"""Extract attribute modifiers from pyfa's eos/effects.py into a TSV table.

eve.db lists which effects each item has, but what an effect *does* (which
attribute it reads, which skill it scales with, what it modifies) only exists
as Python code in eos/effects.py. This script reads that file statically (no
pyfa imports needed) and writes one row per modifier call it can find.

pyfa runs the same handler for different owners and passes the owner kind in
`context` ('ship', 'skill', 'implant', ...). Handlers branch on it (e.g. a
skill multiplies by its own level only in 'skill' context), so each handler
that looks at `context` is evaluated once per context in CONTEXTS. Contexts
giving identical rows are merged; the `contexts` column lists them ('*' when
the handler doesn't look at `context` at all).

Usage:
    python gen_effect_modifiers.py [path/to/eos/effects.py] [output.tsv]

Defaults to the pyfa checkout two levels up and ../PyfaReader/data/effect_modifiers.tsv.
Re-run whenever pyfa's effects.py changes.
"""

import ast
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EFFECTS = os.path.normpath(os.path.join(HERE, '..', '..', 'eos', 'effects.py'))
DEFAULT_OUTPUT = os.path.normpath(os.path.join(HERE, '..', 'PyfaReader', 'data', 'effect_modifiers.tsv'))

COLUMNS = (
    'effectID', 'effectName', 'effectType', 'runTime', 'contexts', 'operation', 'domain', 'target', 'targetAttr',
    'sourceAttr', 'sourceOf', 'factor', 'invert', 'perLevel', 'skill', 'filter', 'condition', 'stackingPenalized',
    'penaltyGroup', 'position', 'valueExpr', 'note',
)

# Owner kinds pyfa passes as `context` (see eos/saveddata/*.py runEffects).
# 'projected' is evaluated together with the projecting item's own kind.
# 'moduleCharge': a charge's effects, run with the *module* as the owner.
CONTEXTS = (
    ('ship',), ('skill',), ('implant',), ('booster',), ('module',), ('moduleCharge',), ('drone',), ('fighter',),
    ('projected', 'module'), ('projected', 'drone'), ('projected', 'fighter'),
)

# method name -> (operation, target). target is 'item' or 'charge'.
METHODS = {
    'boostItemAttr': ('boost', 'item'),
    'multiplyItemAttr': ('multiply', 'item'),
    'increaseItemAttr': ('increase', 'item'),
    'forceItemAttr': ('force', 'item'),
    'preAssignItemAttr': ('preAssign', 'item'),
    'filteredItemBoost': ('boost', 'item'),
    'filteredItemMultiply': ('multiply', 'item'),
    'filteredItemIncrease': ('increase', 'item'),
    'filteredItemForce': ('force', 'item'),
    'filteredItemPreAssign': ('preAssign', 'item'),
    'filteredChargeBoost': ('boost', 'charge'),
    'filteredChargeMultiply': ('multiply', 'charge'),
    'filteredChargeIncrease': ('increase', 'charge'),
    'filteredChargeForce': ('force', 'charge'),
    'filteredChargePreAssign': ('preAssign', 'charge'),
    # module.xxxChargeAttr: the charge loaded in that module
    'boostChargeAttr': ('boost', 'charge'),
    'multiplyChargeAttr': ('multiply', 'charge'),
    'increaseChargeAttr': ('increase', 'charge'),
    'forceChargeAttr': ('force', 'charge'),
    'preAssignChargeAttr': ('preAssign', 'charge'),
    # Direct ModifiedAttributeDict calls: fit.extraAttributes.increase(...),
    # src.itemModifiedAttributes.force(...), ...
    'boost': ('boost', 'item'),
    'multiply': ('multiply', 'item'),
    'increase': ('increase', 'item'),
    'force': ('force', 'item'),
    'preAssign': ('preAssign', 'item'),
}

UNRESOLVED = object()


class Level:
    """Stands for the owner's skill level (skill.level, container.level, ...)."""


LEVEL = Level()


class Env(dict):
    """Local variable bindings to constant values (plus the expressions they came from)."""

    def __init__(self, parent=None, context=()):
        super().__init__(parent or {})
        self.exprs = dict(parent.exprs) if parent is not None else {}
        self.context = parent.context if parent is not None else context
        self.conditions = list(parent.conditions) if parent is not None else []

    def substitute(self, node):
        """Inline local variables assigned from expressions, e.g. bonus = attr * x."""
        exprs = self.exprs

        class Inline(ast.NodeTransformer):
            def visit_Name(self, n):
                return exprs.get(n.id, n)

        return Inline().visit(ast.parse(ast.unparse(node), mode='eval')).body

    def resolve(self, node):
        if isinstance(node, ast.Constant):
            return node.value
        # 'skill' in context
        if (isinstance(node, ast.Compare) and len(node.ops) == 1 and isinstance(node.ops[0], (ast.In, ast.NotIn))
                and ast.unparse(node.comparators[0]) == 'context' and isinstance(node.left, ast.Constant)):
            inside = node.left.value in self.context
            return inside if isinstance(node.ops[0], ast.In) else not inside
        # layer == 'hull' and similar comparisons of known values
        if (isinstance(node, ast.Compare) and len(node.ops) == 1 and isinstance(node.ops[0], (ast.Eq, ast.NotEq))):
            left, right = self.resolve(node.left), self.resolve(node.comparators[0])
            if left is not UNRESOLVED and right is not UNRESOLVED and not isinstance(left, Level) \
                    and not isinstance(right, Level):
                return (left == right) if isinstance(node.ops[0], ast.Eq) else (left != right)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            v = self.resolve(node.operand)
            return not v if isinstance(v, bool) else UNRESOLVED
        if isinstance(node, ast.BoolOp):
            vals = [self.resolve(v) for v in node.values]
            if all(isinstance(v, bool) for v in vals):
                return all(vals) if isinstance(node.op, ast.And) else any(vals)
            return UNRESOLVED
        # a if cond else b
        if isinstance(node, ast.IfExp):
            cond = self.resolve(node.test)
            if isinstance(cond, bool):
                return self.resolve(node.body if cond else node.orelse)
            return UNRESOLVED
        # skill.level / container.level / src.level
        if isinstance(node, ast.Attribute) and node.attr == 'level':
            return LEVEL
        if isinstance(node, ast.Name):
            return self.get(node.id, UNRESOLVED)
        if isinstance(node, (ast.Tuple, ast.List, ast.Set)):
            vals = [self.resolve(e) for e in node.elts]
            return UNRESOLVED if any(v is UNRESOLVED for v in vals) else tuple(vals)
        # 'abc{}'.format(x)
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'format'
                and isinstance(node.func.value, ast.Constant) and not node.keywords):
            args = [self.resolve(a) for a in node.args]
            if any(a is UNRESOLVED for a in args):
                return UNRESOLVED
            return node.func.value.value.format(*args)
        # range(1, 5)
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == 'range'
                and not node.keywords and 1 <= len(node.args) <= 3):
            args = [self.resolve(a) for a in node.args]
            if all(isinstance(a, int) and not isinstance(a, bool) for a in args):
                return tuple(range(*args))
            return UNRESOLVED
        # s[0], s[1:]
        if isinstance(node, ast.Subscript):
            v = self.resolve(node.value)
            if isinstance(v, (str, tuple)):
                sl = node.slice
                if isinstance(sl, ast.Slice):
                    parts = [self.resolve(p) if p is not None else None for p in (sl.lower, sl.upper, sl.step)]
                    if all(p is None or isinstance(p, int) for p in parts):
                        return v[slice(*parts)]
                else:
                    i = self.resolve(sl)
                    if isinstance(i, int) and -len(v) <= i < len(v):
                        return v[i]
            return UNRESOLVED
        # x.capitalize() and similar no-argument string methods
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and not node.args and not node.keywords
                and node.func.attr in ('capitalize', 'lower', 'upper', 'title')):
            v = self.resolve(node.func.value)
            return getattr(v, node.func.attr)() if isinstance(v, str) else UNRESOLVED
        # f-strings
        if isinstance(node, ast.JoinedStr):
            parts = []
            for v in node.values:
                if isinstance(v, ast.Constant):
                    parts.append(str(v.value))
                elif isinstance(v, ast.FormattedValue) and v.format_spec is None:
                    r = self.resolve(v.value)
                    if r is UNRESOLVED:
                        return UNRESOLVED
                    parts.append(str(r))
                else:
                    return UNRESOLVED
            return ''.join(parts)
        # '%sDamage' % x
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mod) and isinstance(node.left, ast.Constant):
            right = self.resolve(node.right)
            if right is not UNRESOLVED:
                try:
                    return node.left.value % right
                except (TypeError, ValueError):
                    return UNRESOLVED
        # 'a' + x
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            left, right = self.resolve(node.left), self.resolve(node.right)
            if isinstance(left, str) and isinstance(right, str):
                return left + right
        return UNRESOLVED


def is_number(node):
    return isinstance(node, ast.Constant) and isinstance(node.value, (int, float)) and not isinstance(node.value, bool)


def split_invert(node):
    """1 / x -> (x, True)."""
    if (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div) and is_number(node.left)
            and node.left.value == 1):
        return node.right, True
    return node, False


def split_factor(node):
    """-x -> (x, -1); x * 10 -> (x, 10); x / 100 -> (x, 0.01); (x or 0) -> (x, 1)."""
    factor = 1
    while True:
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            factor, node = -factor, node.operand
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div) and is_number(node.right):
            factor, node = factor / node.right.value, node.left
        elif (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult)
              and isinstance(node.right, ast.Constant) and isinstance(node.right.value, (int, float))):
            factor, node = factor * node.right.value, node.left
        elif (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult)
              and isinstance(node.left, ast.Constant) and isinstance(node.left.value, (int, float))):
            factor, node = factor * node.left.value, node.right
        # attr or 0 / attr or 1: only matters when the attribute is missing
        elif (isinstance(node, ast.BoolOp) and isinstance(node.op, ast.Or) and len(node.values) == 2
              and isinstance(node.values[1], ast.Constant)):
            node = node.values[0]
        else:
            return node, factor


def split_level(node, env):
    """x * <level> -> (x, True); anything else -> (node, False)."""
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult):
        if env.resolve(node.right) is LEVEL:
            return node.left, True
        if env.resolve(node.left) is LEVEL:
            return node.right, True
    # x * 1 (level expression that resolved to 1 outside skill context)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult) and env.resolve(node.right) == 1:
        return node.left, False
    return node, False


def source_attr(node, env, owner):
    """Find the attribute read via <obj>.getModifiedItemAttr('x') in an expression.

    Returns (attribute name, call node, source) where source says whose attribute it is:
      owner         the item owning the effect
      ship          the fit's ship
      skill:<name>  the character's skill of that name
      charge        the owning module's loaded charge
    or None if the object isn't recognized.
    """
    for n in ast.walk(node):
        if (isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                and n.func.attr in ('getModifiedItemAttr', 'getModifiedChargeAttr') and n.args):
            name = env.resolve(n.args[0])
            if not isinstance(name, str):
                continue
            recv = n.func.value
            text = ast.unparse(recv)
            source = None
            if n.func.attr == 'getModifiedChargeAttr':
                # module.getModifiedChargeAttr('x'): the charge loaded in the owning module
                source = 'charge' if isinstance(recv, ast.Name) and recv.id == owner else None
            elif isinstance(recv, ast.Name) and recv.id == owner:
                source = 'owner'
            elif text == 'fit.ship':
                source = 'ship'
            elif text == owner + '.charge':
                source = 'charge'
            elif (isinstance(recv, ast.Call) and isinstance(recv.func, ast.Attribute) and recv.func.attr == 'getSkill'
                  and ast.unparse(recv.func.value) == 'fit.character' and len(recv.args) == 1):
                skill = env.resolve(recv.args[0])
                if isinstance(skill, str):
                    source = 'skill:' + skill
            return name, n, source
    return None, None, None


def parse_filter(lam, env, owner):
    """Turn a filter lambda into the `filter` column.

    Format: clauses joined by '&' (all must match), each a list of terms joined
    by '|' (any may match). A term is '[charge.]kind[:value]' with kind one of
      skill:<name>    item requires that skill
      ownerSkill      item requires the skill that owns the effect
      group:<name>    item is in that group
      hasAttr:<name>  item has that attribute (non-zero)
    '' means no filter; 'unparsed:<python>' means the lambda wasn't understood.
    """
    if not isinstance(lam, ast.Lambda):
        return 'unparsed:' + ast.unparse(lam)
    body = lam.body
    if isinstance(body, ast.Constant) and body.value is True:
        return ''

    def subject(node):
        # mod.item.xxx -> '', mod.charge.xxx -> 'charge.'
        text = ast.unparse(node)
        if '.charge.' in text + '.' or text.endswith('.charge'):
            return 'charge.'
        return ''

    def terms(node):
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.Or):
            out = []
            for v in node.values:
                t = terms(v)
                if t is None:
                    return None
                out.extend(t)
            return out
        # x.item.requiresSkill('Skill') / x.item.requiresSkill(skill)
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'requiresSkill'
                and len(node.args) == 1):
            pre = subject(node.func.value)
            if isinstance(node.args[0], ast.Name) and node.args[0].id == owner:
                return [pre + 'ownerSkill']
            v = env.resolve(node.args[0])
            return [pre + 'skill:' + v] if isinstance(v, str) else None
        # x.item.group.name == 'G'  /  x.item.group.name in (...)
        if (isinstance(node, ast.Compare) and len(node.ops) == 1
                and ast.unparse(node.left).endswith('.group.name')):
            pre = subject(node.left)
            v = env.resolve(node.comparators[0])
            if isinstance(node.ops[0], ast.Eq) and isinstance(v, str):
                return [pre + 'group:' + v]
            if isinstance(node.ops[0], ast.In) and isinstance(v, tuple) and all(isinstance(x, str) for x in v):
                return [pre + 'group:' + x for x in v]
        # 'attr' in x.item.attributes / 'attr' in x.itemModifiedAttributes (same keys)
        if (isinstance(node, ast.Compare) and len(node.ops) == 1 and isinstance(node.ops[0], ast.In)
                and ast.unparse(node.comparators[0]).endswith(('.attributes', '.itemModifiedAttributes'))):
            v = env.resolve(node.left)
            return [subject(node.comparators[0]) + 'hasAttr:' + v] if isinstance(v, str) else None
        # x.item.getAttribute('attr') used as a truth value
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'getAttribute'
                and len(node.args) == 1):
            v = env.resolve(node.args[0])
            return [subject(node.func.value) + 'hasAttr:' + v] if isinstance(v, str) else None
        return None

    clauses = body.values if isinstance(body, ast.BoolOp) and isinstance(body.op, ast.And) else [body]
    out = []
    for clause in clauses:
        t = terms(clause)
        if t is None or any('|' in x or '&' in x for x in t):
            return 'unparsed:' + ast.unparse(body)
        out.append('|'.join(t))
    return '&'.join(out)


def parse_condition(test, env, owner=''):
    """An `if` test on the fit, as a `condition` term.

      ship.skill:<name>        the ship requires that skill
      ship.hasAttr:<name>      the ship type has that attribute
      self.baseNonNeg:<name>   the owner's unmodified attribute exists and is >= 0
      unparsed:<python>        anything else (runtime state etc.)
    A leading '!' negates (the else branch).
    """
    test = env.substitute(test)
    # x is not None and x >= 0, with x = owner.item.getAttribute('attr')
    if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.And) and len(test.values) == 2:
        a, b = test.values
        raw = f'{owner}.item.getAttribute('
        if (isinstance(a, ast.Compare) and isinstance(a.ops[0], ast.IsNot) and isinstance(b, ast.Compare)
                and isinstance(b.ops[0], ast.GtE) and is_number(b.comparators[0]) and b.comparators[0].value == 0
                and ast.unparse(a.left) == ast.unparse(b.left) and ast.unparse(a.left).startswith(raw)):
            attr = env.resolve(a.left.args[0])
            if isinstance(attr, str):
                return 'self.baseNonNeg:' + attr
    # fit.ship.item.requiresSkill('Capital Ships')
    if (isinstance(test, ast.Call) and isinstance(test.func, ast.Attribute) and test.func.attr == 'requiresSkill'
            and ast.unparse(test.func.value) == 'fit.ship.item' and len(test.args) == 1):
        v = env.resolve(test.args[0])
        if isinstance(v, str):
            return 'ship.skill:' + v
    # 'shipBonusGI' in fit.ship.item.attributes
    if (isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.ops[0], ast.In)
            and ast.unparse(test.comparators[0]) == 'fit.ship.item.attributes'):
        v = env.resolve(test.left)
        if isinstance(v, str):
            return 'ship.hasAttr:' + v
    return 'unparsed:' + ast.unparse(test)


def negate_condition(cond):
    if cond.startswith('unparsed:'):
        return 'unparsed:not (' + cond[9:] + ')'
    return cond[1:] if cond.startswith('!') else '!' + cond


class HandlerWalker:
    def __init__(self, effect_id, effect_name, effect_type, owner, run_time='normal', buff_value=None):
        self.run_time = run_time  # early | normal | late: pyfa runs effects in these passes
        self.buff_value = buff_value  # warfare buffs: name of the variable holding the buff value
        self.effect_id = effect_id
        self.effect_name = effect_name
        self.effect_type = effect_type
        self.owner = owner  # name of the handler parameter holding the effect's owner
        self.rows = []

    def walk(self, stmts, env):
        stmts = list(stmts)
        for i, st in enumerate(stmts):
            # An `if` we can't decide statically splits the path: the rest of
            # the handler runs once per branch, each under its own condition.
            if isinstance(st, ast.If) and not isinstance(env.resolve(st.test), bool):
                cond = parse_condition(st.test, env, self.owner)
                rest = stmts[i + 1:]
                for branch, negate in ((st.body, False), (st.orelse, True)):
                    inner = Env(env)
                    inner.conditions = env.conditions + [negate_condition(cond) if negate else cond]
                    self.walk(branch + rest, inner)
                return
            self.stmt(st, env)

    def stmt(self, st, env):
        if isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
            env[st.targets[0].id] = env.resolve(st.value)
            env.exprs[st.targets[0].id] = env.substitute(st.value)
            self.scan_calls(st.value, env)
        elif isinstance(st, ast.If):
            cond = env.resolve(st.test)
            if cond is True:
                self.walk(st.body, env)
            elif cond is False:
                self.walk(st.orelse, env)
            else:
                self.walk(st.body, Env(env))
                self.walk(st.orelse, Env(env))
        elif isinstance(st, ast.For):
            # for x in (...) / for a, b in ((..), (..)): unroll over constant tuples
            names = [st.target.id] if isinstance(st.target, ast.Name) else \
                [e.id if isinstance(e, ast.Name) else None for e in st.target.elts] \
                if isinstance(st.target, ast.Tuple) else [None]
            values = env.resolve(st.iter)
            if isinstance(values, tuple):
                for v in values:
                    inner = Env(env)
                    parts = (v,) if isinstance(st.target, ast.Name) else v
                    if not isinstance(parts, tuple) or len(parts) != len(names):
                        parts = (UNRESOLVED,) * len(names)
                    for name, part in zip(names, parts):
                        if name:
                            inner[name] = part
                            inner.exprs.pop(name, None)
                    self.walk(st.body, inner)
            else:
                inner = Env(env)
                for name in names:
                    if name:
                        inner[name] = UNRESOLVED
                self.walk(st.body, inner)
        elif isinstance(st, (ast.With, ast.Try)):
            self.walk(st.body, env)
            for h in getattr(st, 'handlers', []):
                self.walk(h.body, Env(env))
        elif isinstance(st, ast.Expr):
            self.scan_calls(st.value, env)

    def scan_calls(self, node, env):
        for n in ast.walk(node):
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in METHODS:
                self.modifier(n, env)

    def modifier(self, call, env):
        method = call.func.attr
        operation, target = METHODS[method]
        receiver = call.func.value
        receiver_text = ast.unparse(receiver)
        if isinstance(receiver, ast.Name) and receiver.id == self.owner:
            domain = 'self'  # the effect's own item (target 'charge': its loaded charge)
        elif receiver_text == f'{self.owner}.itemModifiedAttributes':
            domain = 'self'
        elif receiver_text == f'{self.owner}.chargeModifiedAttributes':
            domain, target = 'self', 'charge'
        elif receiver_text == 'fit.ship.itemModifiedAttributes':
            domain = 'ship'
        elif method in ('boost', 'multiply', 'increase', 'force', 'preAssign') and receiver_text != 'fit.extraAttributes':
            return  # some other dict/object with a method of the same name
        else:
            domain = ast.unparse(receiver)
            domain = domain[4:] if domain.startswith('fit.') else domain

        args = list(call.args)
        filter_text = ''
        if method.startswith('filtered'):
            if not args:
                return
            filter_text = parse_filter(args.pop(0), env, self.owner)
        if len(args) < 2:
            return
        target_attr = env.resolve(args[0])
        value_node, factor = split_factor(env.substitute(args[1]))
        value_node, invert = split_invert(value_node)
        value_node, factor2 = split_factor(value_node)
        value_node, per_level = split_level(value_node, env)
        value_node, factor3 = split_factor(value_node)
        # amount = factor * v (or factor / v when inverted), v = sourceAttr [* level]
        factor = factor / (factor2 * factor3) if invert else factor * factor2 * factor3
        src_attr, src_call, source_of = source_attr(value_node, env, self.owner)
        if self.buff_value and isinstance(value_node, ast.Name) and value_node.id == self.buff_value:
            # warfare buff table: the amount is the buff's value
            src_attr, src_call, source_of = 'warfareBuffValue', value_node, 'buff'

        kw = {k.arg: k.value for k in call.keywords if k.arg}
        skill = env.resolve(kw['skill']) if 'skill' in kw else None
        penalized = env.resolve(kw['stackingPenalties']) if 'stackingPenalties' in kw else False
        penalty_group = env.resolve(kw['penaltyGroup']) if 'penaltyGroup' in kw else 'default'
        position = env.resolve(kw['position']) if 'position' in kw else ('pre' if operation == 'increase' else '')

        notes = []
        if penalty_group is None or not penalized:
            penalty_group = 'default'
        if not isinstance(penalty_group, str):
            notes.append('penaltyGroup=' + ast.unparse(kw['penaltyGroup']))
            penalty_group = 'default'
        if not isinstance(position, str):
            notes.append('position=' + ast.unparse(kw['position']))
            position = 'pre'
        if not isinstance(target_attr, str):
            notes.append('targetAttr=' + ast.unparse(args[0]))
            target_attr = ''
        if skill is UNRESOLVED:
            notes.append('skill=' + ast.unparse(kw['skill']))
            skill = None
        if penalized is UNRESOLVED:
            notes.append('stackingPenalties=' + ast.unparse(kw['stackingPenalties']))
            penalized = False
        if invert and per_level:
            notes.append('level inside 1/x')
        if src_attr is None:
            notes.append('no source attribute')
        elif source_of is None:
            notes.append('source=' + ast.unparse(src_call.func.value))
        value_expr = '' if ast.unparse(value_node) == ast.unparse(src_call or value_node) and src_call else ast.unparse(value_node)

        self.rows.append({
            'effectID': self.effect_id,
            'effectName': self.effect_name,
            'effectType': self.effect_type,
            'runTime': self.run_time,
            'operation': operation,
            'domain': domain,
            'target': target,
            'targetAttr': target_attr,
            'sourceAttr': src_attr or '',
            'sourceOf': source_of or '',
            'factor': factor,
            'invert': int(invert),
            'perLevel': int(per_level),
            'skill': skill or '',
            'filter': filter_text,
            'condition': '&'.join(env.conditions),
            'stackingPenalized': int(bool(penalized)),
            'penaltyGroup': penalty_group if penalized else '',
            'position': position,
            'valueExpr': value_expr,
            'note': '; '.join(notes),
        })


def effect_name(cls):
    doc = ast.get_docstring(cls)
    return doc.strip().splitlines()[0].strip() if doc else ''


def class_constant(cls, name):
    """A class-level `name = 'constant'`, or None."""
    for st in cls.body:
        if (isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name)
                and st.targets[0].id == name and isinstance(st.value, ast.Constant)):
            return st.value.value
    return None


def effect_type(cls):
    """The class-level `type = ...` ('passive', 'active', ('projected', 'active'), ...), '|'-joined."""
    for st in cls.body:
        if (isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name)
                and st.targets[0].id == 'type'):
            v = Env().resolve(st.value)
            if isinstance(v, str):
                return v
            if isinstance(v, tuple):
                return '|'.join(str(x) for x in v)
    return ''


def handler_rows(cls, handler):
    """Rows for one effect, evaluated per context; identical results are merged."""
    eid, name, etype = int(cls.name[6:]), effect_name(cls), effect_type(cls)
    run_time = class_constant(cls, 'runTime') or 'normal'
    uses_context = any(isinstance(n, ast.Name) and n.id == 'context' for n in ast.walk(handler))
    contexts = CONTEXTS if uses_context else ((),)

    groups = []  # [(row list, [context labels])]
    owner = handler.args.args[1].arg if len(handler.args.args) > 1 else ''
    for ctx in contexts:
        walker = HandlerWalker(eid, name, etype, owner, run_time)
        walker.walk(handler.body, Env(context=ctx))
        label = '+'.join(ctx) if ctx else '*'
        for rows, labels in groups:
            if rows == walker.rows:
                labels.append(label)
                break
        else:
            groups.append((walker.rows, [label]))

    out = []
    for rows, labels in groups:
        for r in rows:
            out.append(dict(r, contexts='|'.join(labels)))
    return out


def warfare_buff_rows(fit_py_path):
    """Rows for each warfare buff in pyfa's Fit.__runCommandBoosts.

    That method applies command burst buffs by ID in a long
    `if warfareBuffID == N: ... elif ...` chain, written with the same calls as
    effect handlers (self.ship.boostItemAttr, self.modules.filteredItemBoost, ...)
    with the buff's `value` as the amount. Each branch becomes rows with
    effectID = buff ID, sourceOf 'buff'.
    """
    with open(fit_py_path, encoding='utf-8') as f:
        tree = ast.parse(f.read())
    method = next(n for n in ast.walk(tree)
                  if isinstance(n, ast.FunctionDef) and n.name == '__runCommandBoosts')

    class SelfToFit(ast.NodeTransformer):
        def visit_Name(self, n):
            return ast.copy_location(ast.Name(id='fit', ctx=n.ctx), n) if n.id == 'self' else n

    rows = []
    for node in ast.walk(method):
        if not (isinstance(node, ast.If) and isinstance(node.test, ast.Compare)
                and ast.unparse(node.test.left) == 'warfareBuffID' and isinstance(node.test.ops[0], ast.Eq)
                and is_number(node.test.comparators[0])):
            continue
        buff_id = node.test.comparators[0].value
        comment = ''
        body = [SelfToFit().visit(ast.parse(ast.unparse(st))).body[0] for st in node.body]
        walker = HandlerWalker(buff_id, f'warfareBuff{buff_id}', 'gang', owner='', buff_value='value')
        walker.walk(body, Env())
        for r in walker.rows:
            rows.append(dict(r, contexts='*', effectName=comment or r['effectName']))
    return rows


def write_rows(path, rows):
    def clean(v):
        return str(v).replace('\t', ' ').replace('\n', ' ')

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(COLUMNS) + '\n')
        for r in rows:
            f.write('\t'.join(clean(r[c]) for c in COLUMNS) + '\n')
    return sum(1 for r in rows if r['note'] or r['valueExpr'] or r['filter'].startswith('unparsed:')
               or 'unparsed:' in r['condition'])


def main():
    effects_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EFFECTS
    output_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUTPUT

    with open(effects_path, encoding='utf-8') as f:
        tree = ast.parse(f.read())

    rows = []
    effects = 0
    for cls in tree.body:
        if not (isinstance(cls, ast.ClassDef) and cls.name.startswith('Effect') and cls.name[6:].isdigit()):
            continue
        handler = next((f for f in cls.body if isinstance(f, ast.FunctionDef) and f.name == 'handler'), None)
        if handler is None:
            continue
        effects += 1
        rows.extend(handler_rows(cls, handler))
    flagged = write_rows(output_path, rows)
    print(f'{effects} effects -> {len(rows)} modifiers ({flagged} only partly resolved) -> {output_path}')

    # Command burst buffs live in eos/saveddata/fit.py, next to effects.py's package.
    fit_py = os.path.join(os.path.dirname(effects_path), 'saveddata', 'fit.py')
    buff_path = os.path.join(os.path.dirname(output_path), 'warfare_buffs.tsv')
    buff_rows = warfare_buff_rows(fit_py)
    flagged = write_rows(buff_path, buff_rows)
    print(f'{len({r["effectID"] for r in buff_rows})} warfare buffs -> {len(buff_rows)} modifiers '
          f'({flagged} only partly resolved) -> {buff_path}')


if __name__ == '__main__':
    main()
