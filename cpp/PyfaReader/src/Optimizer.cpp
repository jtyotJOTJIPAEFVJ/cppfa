#include "Optimizer.h"

#include "PyMath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <numeric>
#include <ostream>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fitengine::opt
{
const char* rackName(int rack)
{
    static const char* names[RackCount] = {"mid", "low", "rig"};
    return rack >= 0 && rack < RackCount ? names[rack] : "?";
}

const char* resourceName(int resource)
{
    static const char* names[ResourceCount] = {"cpu", "power", "calibration"};
    return resource >= 0 && resource < ResourceCount ? names[resource] : "?";
}

namespace
{
bool isAssaultDamageControl(const GameData& gd, const Type& type)
{
    static const int effect = [&] {
        int id = gd.effectID("moduleBonusAssaultDamageControl");
        return id ? id : 7012;
    }();
    return type.hasEffect(effect);
}
}  // namespace

bool excludedFromSearch(const GameData& gd, const Type& type)
{
    return isAssaultDamageControl(gd, type);
}

State clampPinnedState(const GameData& gd, const Type& type, State state)
{
    if (isAssaultDamageControl(gd, type) && state > State::Online)
        return State::Online;
    return state;
}

namespace
{
using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

constexpr double kTiny = 1e-6;
constexpr int kMaxRackSlots = 8;
constexpr int kPlanItemBudget = 480;  // options + their charges per measurement plan (kMaxItems is 512)
// Stage 2 local search: accepted moves per starting set at most. The search provably
// ends without it (each move strictly improves a strict weak order); it is a backstop.
constexpr size_t kStage2MaxMoves = 10000;

const Slot kRackSlot[RackCount] = {Slot::Med, Slot::Low, Slot::Rig};

int layerOf(Goal g)
{
    switch (g)
    {
    case Goal::ShieldPassive:
    case Goal::ShieldActive: return Shield;
    case Goal::ArmorPassive:
    case Goal::ArmorActive: return Armor;
    default: return Hull;
    }
}
bool isActiveGoal(Goal g) { return g == Goal::ShieldActive || g == Goal::ArmorActive; }

// ShipAttrs field indices.
int shipField(Attr a)
{
    for (size_t i = 0; i < std::size(fitschema::ShipAttrs::kFields); ++i)
        if (fitschema::ShipAttrs::kFields[i] == a)
            return static_cast<int>(i);
    return -1;
}

const Attr kResAttrs[LayerCount][4] = {
    {Attr::shieldEmDamageResonance, Attr::shieldThermalDamageResonance, Attr::shieldKineticDamageResonance,
     Attr::shieldExplosiveDamageResonance},
    {Attr::armorEmDamageResonance, Attr::armorThermalDamageResonance, Attr::armorKineticDamageResonance,
     Attr::armorExplosiveDamageResonance},
    {Attr::emDamageResonance, Attr::thermalDamageResonance, Attr::kineticDamageResonance,
     Attr::explosiveDamageResonance}};
const Attr kHpAttr[LayerCount] = {Attr::shieldCapacity, Attr::armorHP, Attr::hp};
const Attr kOutputAttrs[3] = {Attr::cpuOutput, Attr::powerOutput, Attr::upgradeCapacity};

uint64_t shipMask(std::initializer_list<Attr> attrs)
{
    uint64_t m = 0;
    for (Attr a : attrs)
        if (int f = shipField(a); f >= 0)
            m |= uint64_t{1} << f;
    return m;
}

// Repair of one layer from one module, split like pyfa's sustainable tank:
// fixed + sustainability * gain, using capUse GJ/s at full sustainability.
struct RepItem
{
    double fixed = 0, gain = 0, capUse = 0;
};
RepItem decompose(const ModuleStats& m, int layer)
{
    RepItem it;
    if (!m.repEffects)
        return it;
    RepProfile p = makeRepProfile(&m, 1, 0.0f, 0.0f);
    it.fixed = p.extra[static_cast<size_t>(layer)] + p.adjust[static_cast<size_t>(layer)];
    for (int k = 0; k < p.repCount; ++k)
    {
        const RepProfile::Rep& r = p.reps[static_cast<size_t>(k)];
        if (r.layer != layer)
            continue;
        it.gain = r.hasCharge ? r.amount * r.onDuty * r.paste / r.cycleSeconds : r.amount / r.cycleSeconds;
        it.capUse = r.capUse;
    }
    return it;
}

// Unconstrained repair (infinite capacitor) of a module for a layer.
double repStrength(const ModuleStats& m, int layer)
{
    RepItem it = decompose(m, layer);
    return it.fixed + it.gain;
}

// Fractional knapsack: an upper bound on pyfa's greedy capacitor split.
struct KnapItem
{
    double gain, capUse;
    int count;
};
double fractionalKnapsack(double budget, std::vector<KnapItem>& items)
{
    double value = 0;
    std::vector<KnapItem*> paid;
    for (KnapItem& it : items)
    {
        if (it.gain <= 0 || it.count <= 0)
            continue;
        if (it.capUse <= 0)
            value += it.gain * it.count;
        else
            paid.push_back(&it);
    }
    std::sort(paid.begin(), paid.end(),
              [](const KnapItem* a, const KnapItem* b) { return a->gain / a->capUse > b->gain / b->capUse; });
    for (KnapItem* it : paid)
        for (int c = 0; c < it->count && budget > 0; ++c)
        {
            double frac = std::min(1.0, budget / it->capUse);
            value += frac * it->gain;
            budget -= it->capUse;
        }
    return value;
}

// ------------------------------------------------------------------ candidates

enum class Role : uint8_t
{
    Core,
    Resist,
    Fitting,
};
const char* roleName(Role r) { return r == Role::Core ? "core" : r == Role::Resist ? "resist" : "fitting"; }

struct Candidate
{
    ModuleInput input;
    const Type* type = nullptr;
    int rack = 0;
    int groupID = 0;
    int maxGroup = 0, maxType = 0;
    double price = std::numeric_limits<double>::infinity();
    bool hasPrice = false;
    Engine::Reach reach;

    // Measured alone on the pinned fit (deltas against the pinned fit alone).
    double goalGain = 0, dividerDrop = 0, hpGain = 0, capGain = 0, outputGain = 0;
    double rep = 0;  // unconstrained repair for the goal layer
    double cpu = 0, power = 0, calibration = 0, capUse = 0;
    uint16_t resMask = 0;  // which of the 12 ship resonances it changes
    std::array<double, 3> outputDelta{};
    // How much it lowers the pinned modules' CPU / power / calibration (>= 0): the
    // change of the fit's total use minus its own cost.
    std::array<double, 3> pinnedDrop{};

    uint64_t shipChanged = 0;  // ShipAttrs fields it changes alone

    // Pair gains (it modifies other modules).
    double pairGain = 0, costGain = 0;

    bool useful = false;
    Role role = Role::Core;
    int subgroup = -1;
    std::array<double, 6> strength{};
};

struct Subgroup
{
    int rack = 0;
    Role role = Role::Core;
    std::string key;
    std::vector<int> members;  // candidate indices, strongest first
    Influence influence;       // of the representative; optionFields hold subgroup indices
};

std::string sizeToken(const std::string& name)
{
    static const std::regex mm(R"((\d+)mm)");
    std::smatch m;
    if (std::regex_search(name, m, mm))
        return m[1].str() + "mm";
    static const char* words[] = {"X-Large", "Micro", "Small", "Medium", "Large", "Heavy", "Capital", "Huge"};
    for (const char* w : words)
    {
        size_t at = name.find(w);
        while (at != std::string::npos)
        {
            bool startOk = at == 0 || name[at - 1] == ' ' || name[at - 1] == '\'';
            size_t end = at + std::strlen(w);
            bool endOk = end == name.size() || name[end] == ' ';
            if (startOk && endOk && !(std::string(w) == "Large" && at >= 2 && name.compare(at - 2, 2, "X-") == 0))
                return w;
            at = name.find(w, at + 1);
        }
    }
    return "";
}

std::string rigFamily(const std::string& name)
{
    if (name.size() > 3 && name.compare(name.size() - 3, 3, " II") == 0)
        return name.substr(0, name.size() - 3);
    if (name.size() > 2 && name.compare(name.size() - 2, 2, " I") == 0)
        return name.substr(0, name.size() - 2);
    return name;
}

// ------------------------------------------------------------------ small multisets

struct Multiset
{
    uint8_t n = 0;
    std::array<uint8_t, kMaxRackSlots> k{};  // local kind indices, ascending
};

// Every multiset of 0..maxSize items from `count` kinds, respecting per-kind caps.
std::vector<Multiset> enumerateMultisets(int count, int maxSize, const std::vector<int>& cap)
{
    std::vector<Multiset> out;
    Multiset cur;
    std::vector<int> used(static_cast<size_t>(count), 0);
    std::function<void(int)> rec = [&](int from) {
        out.push_back(cur);
        if (cur.n >= maxSize)
            return;
        for (int i = from; i < count; ++i)
        {
            if (used[static_cast<size_t>(i)] >= cap[static_cast<size_t>(i)])
                continue;
            ++used[static_cast<size_t>(i)];
            cur.k[cur.n++] = static_cast<uint8_t>(i);
            rec(i);
            --cur.n;
            --used[static_cast<size_t>(i)];
        }
    };
    rec(0);
    return out;
}

// Packed counts of an influencer list: 4 bits per entry, up to 32 entries.
struct Key
{
    uint64_t a = 0, b = 0;
    bool operator==(const Key& o) const { return a == o.a && b == o.b; }
};
struct KeyHash
{
    size_t operator()(const Key& k) const
    {
        uint64_t h = k.a * 0x9E3779B97F4A7C15ull ^ (k.b + 0x632BE59BD9B4E019ull + (k.a << 6) + (k.a >> 2));
        return static_cast<size_t>(h ^ (h >> 29));
    }
};
Key packCounts(const std::vector<int>& list, const uint8_t* counts)
{
    Key key;
    for (size_t i = 0; i < list.size(); ++i)
    {
        uint64_t c = counts[list[i]];
        if (i < 16)
            key.a |= c << (4 * i);
        else
            key.b |= c << (4 * (i - 16));
    }
    return key;
}

// Memo of values that are pure functions of their key (engine measurements), shared
// by worker threads. get(key, make) computes each entry once: other threads asking
// for it meanwhile wait for it. Lookups of finished entries take no lock (open
// addressing over atomic node pointers; a grown table replaces the old one, which
// stays readable); new entries lock the key's shard. Values never move.
template <class V>
class Cache
{
public:
    template <class F>
    const V& get(const Key& k, F&& make)
    {
        const uint64_t h = hash(k);
        Shard& s = shard(h);
        Node* n = nullptr;
        if (const Table* t = s.table.load(std::memory_order_acquire))
            n = probe(*t, h, k);
        if (n && n->ready.load(std::memory_order_acquire))
            return n->value;
        if (!n)
            n = &add(s, h, k);
        std::call_once(n->once, [&] {
            n->value = make();
            n->ready.store(true, std::memory_order_release);
        });
        return n->value;
    }

private:
    static constexpr size_t kShards = 64;
    struct Node
    {
        Node(const Key& k, uint64_t h) : key(k), hash(h) {}
        Key key;
        uint64_t hash;
        std::once_flag once;
        std::atomic<bool> ready{false};
        V value{};
    };
    struct Table
    {
        explicit Table(size_t size) : mask(size - 1), slots(std::make_unique<std::atomic<Node*>[]>(size)) {}
        size_t mask;
        std::unique_ptr<std::atomic<Node*>[]> slots;
    };
    struct Shard
    {
        std::mutex m;
        std::atomic<Table*> table{nullptr};
        std::vector<std::unique_ptr<Table>> tables;  // the current one last; older ones stay readable
        std::deque<Node> nodes;
    };
    static uint64_t hash(const Key& k) { return static_cast<uint64_t>(KeyHash{}(k)) * 0xD6E8FEB86659FD93ull; }
    Shard& shard(uint64_t h) { return shards_[h >> 58 & (kShards - 1)]; }
    static Node* probe(const Table& t, uint64_t h, const Key& k)
    {
        for (size_t i = (h >> 20) & t.mask;; i = (i + 1) & t.mask)
        {
            Node* n = t.slots[i].load(std::memory_order_acquire);
            if (!n)
                return nullptr;
            if (n->hash == h && n->key == k)
                return n;
        }
    }
    static void place(Table& t, Node& n)
    {
        size_t i = (n.hash >> 20) & t.mask;
        while (t.slots[i].load(std::memory_order_relaxed))
            i = (i + 1) & t.mask;
        t.slots[i].store(&n, std::memory_order_release);
    }
    // The key's node, added (not computed yet) if missing.
    Node& add(Shard& s, uint64_t h, const Key& k)
    {
        std::lock_guard lock(s.m);
        Table* t = s.table.load(std::memory_order_relaxed);
        if (t)
            if (Node* n = probe(*t, h, k))
                return *n;
        if (!t || 2 * (s.nodes.size() + 1) > t->mask + 1)
        {
            // Grow: a new table with every node, published as a whole.
            auto grown = std::make_unique<Table>(t ? 2 * (t->mask + 1) : 16);
            for (Node& n : s.nodes)
                place(*grown, n);
            s.table.store(grown.get(), std::memory_order_release);
            s.tables.push_back(std::move(grown));
            t = s.tables.back().get();
        }
        Node& n = s.nodes.emplace_back(k, h);
        place(*t, n);
        return n;
    }
    std::unique_ptr<Shard[]> shards_ = std::make_unique<Shard[]>(kShards);
};

// Fixed worker threads for data-parallel loops. run(n, grain, fn) calls fn(i, worker)
// for every i < n (dynamic chunks of `grain`); the calling thread is worker 0.
class Pool
{
public:
    explicit Pool(int threads)
    {
        for (int i = 1; i < threads; ++i)
            threads_.emplace_back([this, i] { loop(i); });
    }
    ~Pool()
    {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_)
            t.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    int size() const { return static_cast<int>(threads_.size()) + 1; }

    void run(size_t n, size_t grain, const std::function<void(size_t, int)>& fn)
    {
        grain = std::max<size_t>(grain, 1);
        if (threads_.empty() || n <= grain)
        {
            for (size_t i = 0; i < n; ++i)
                fn(i, 0);
            return;
        }
        {
            std::lock_guard lock(m_);
            job_ = &fn;
            n_ = n;
            grain_ = grain;
            next_ = 0;
            busy_ = static_cast<int>(threads_.size());
            error_ = nullptr;
            ++generation_;
        }
        wake_.notify_all();
        work(0);
        std::unique_lock lock(m_);
        done_.wait(lock, [&] { return busy_ == 0; });
        job_ = nullptr;
        if (error_)
            std::rethrow_exception(error_);
    }

private:
    void work(int worker)
    {
        for (;;)
        {
            size_t first = next_.fetch_add(grain_);
            if (first >= n_)
                return;
            size_t last = std::min(n_, first + grain_);
            try
            {
                for (size_t i = first; i < last; ++i)
                    (*job_)(i, worker);
            }
            catch (...)
            {
                std::lock_guard lock(m_);
                if (!error_)
                    error_ = std::current_exception();
                next_ = n_;
            }
        }
    }
    void loop(int worker)
    {
        uint64_t seen = 0;
        for (;;)
        {
            {
                std::unique_lock lock(m_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_)
                    return;
                seen = generation_;
            }
            work(worker);
            std::lock_guard lock(m_);
            if (--busy_ == 0)
                done_.notify_all();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    const std::function<void(size_t, int)>* job_ = nullptr;
    size_t n_ = 0, grain_ = 1;
    std::atomic<size_t> next_{0};
    int busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::exception_ptr error_;
};
}  // namespace

namespace
{
// Goals within ~1e-7 relative are float noise between variants; they compare by
// price. The goal is bucketed (a fixed grid on log(goal)) rather than compared with
// a tolerance: "within tolerance" is not transitive, and the local search cycled
// on it (A -> cheaper B within 1e-7 -> cheaper C within 1e-7 -> A, C being more
// than 1e-7 below A). Buckets make the key a strict weak order, so every accepted
// move strictly improves it and the search terminates. Goals that are not > 0
// (including NaN) share the lowest bucket.
int64_t goalBucket(double goal)
{
    if (!(goal > 0) || !std::isfinite(goal))
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(std::floor(std::log(goal) * 1e7));
}

struct Scored
{
    bool valid = false;
    bool fits = false;
    double goal = 0;
    int64_t bucket = std::numeric_limits<int64_t>::min();
    double price = 0;
    TestResult r;
};
// Lexicographic (valid, fits, goal bucket, cheaper): a strict weak order.
bool better(const Scored& a, const Scored& b)
{
    if (a.valid != b.valid)
        return a.valid;
    if (a.fits != b.fits)
        return a.fits;
    if (a.bucket != b.bucket)
        return a.bucket > b.bucket;
    return a.price < b.price;
}
// A limit of stage 1's model (key sizes, plan size). The search then skips stages 1
// and 2: refine, which evaluates exactly and needs no model, searches from the empty set.
struct ModelLimit : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

bool fitsOf(const TestResult& t)
{
    return t.cpuUsed <= t.cpuOutput && t.powerUsed <= t.powerOutput && t.calibrationUsed <= t.calibrationOutput;
}

// One search: everything derived from the fit, the goal and the options.
class Search
{
public:
    Search(const GameData& gd, const Engine& engine, const FitInput& fit, const Options& opt)
        : gd_(gd), engine_(engine), opt_(opt), fit_(fit), log_(opt.log), goal_(opt.goal), layer_(layerOf(opt.goal)),
          active_(isActiveGoal(opt.goal))
    {
    }

    Result run();

private:
    // ---- setup and candidates
    void setup();
    void gatherCandidates();
    void measure();
    void interactions();
    void classify();

    // ---- infeasibility (Result::infeasible): see the comments at the definitions
    bool pinnedOver() const;
    bool diagnoseCalibration();
    bool diagnoseOutputs();
    void diagnoseSearch(bool found);
    std::string fmt(double v) const;
    std::string selectionNote() const;
    std::array<double, 3> pinnedUsed_{}, pinnedOutput_{};  // the pinned part alone (open slots empty)
    std::vector<int> pinnedIndex_;  // base_.modules index -> fit_.modules index
    std::vector<Shortfall> bounds_;  // diagnoseOutputs' bounds per over resource (for diagnoseSearch)
    void noGoalWarning();

    // ---- model
    void buildKinds();
    void buildCorePlan();
    ModuleStats kindRecord(int kind, const uint8_t* counts);
    ModuleStats pinnedRecord(int pinned, const uint8_t* counts);
    float capPeakRecord(const uint8_t* counts);
    float hpRecord(const uint8_t* counts);
    double budgetRecord(const uint8_t* counts);
    void evaluateCore(const uint8_t* projection, TestDetail& d);
    struct CoreEval
    {
        RepProfile profile;
        double v0 = 0;
        double cpu = 0, power = 0, calibration = 0;
    };
    void assemble(const uint8_t* counts, CoreEval& out);
    double coreValue(const CoreEval& ce, double extraCap) const
    {
        return active_ ? ce.profile.repair(layer_, extraCap) : ce.v0;
    }

    // resist side
    void buildResistSets();
    struct ResistRec
    {
        float d = 0, x = 0, cpu = 0, power = 0, calibration = 0;
    };
    struct ResistSlot
    {
        ResistRec rec;
        std::atomic<bool> ready{false};
    };
    // The resist sets next to one combination of eps kinds. Each set has a lower
    // bound of its D, from the engine's own resonance composition of the eps modules
    // and of each resist kind alone (resistBounds); exact records are evaluated with
    // the resist plan when a set is first needed.
    struct ResistTable
    {
        static constexpr size_t kChunk = 1024;
        std::vector<uint8_t> counts;  // eps counts (other kinds 0)
        bool exact = false;           // every set evaluated at build: the bounds are exact
        std::vector<std::array<int, RackCount>> groupSize;       // per rack size vector...
        std::vector<std::vector<std::pair<float, int>>> groups;  // ...its sets: (bound, set), ascending
        std::vector<std::atomic<ResistSlot*>> chunks;            // records by set, kChunk per chunk
        std::mutex m;
        ~ResistTable()
        {
            for (auto& c : chunks)
                delete[] c.load();
        }
    };
    struct ResistTableSlot
    {
        std::once_flag once;
        std::unique_ptr<ResistTable> table;
    };
    // The sets that fit some free slots in (D, set) order, materialized on demand:
    // the groups' bound-sorted lists are merged, and a set is evaluated when its
    // bound comes up; an evaluated set is emitted once no unevaluated bound is below
    // its D. Same order as sorting all sets by exact D, stable.
    struct ResistOrder
    {
        ResistTable* table = nullptr;
        std::mutex m;
        std::atomic<size_t> size{0};                // emitted so far
        std::vector<std::unique_ptr<int[]>> chunks;  // emitted sets, ResistTable::kChunk per chunk
        struct Cursor
        {
            float bound;
            int set, group;
            size_t pos;
        };
        std::vector<Cursor> pending;               // min-heap by (bound, set): each group's next set
        std::vector<std::pair<float, int>> ready;  // min-heap by (D, set): evaluated, not emitted
        std::vector<float> inFlight;               // bounds of the sets being evaluated
        std::condition_variable cv;                // an evaluation finished
    };
    ResistTable& resistTable(const uint8_t* counts);
    void buildResistTable(ResistTable& t);
    bool resistBounds(ResistTable& t, const std::array<int, RackCount>& epsCount, std::vector<float>& bound);
    void evaluateResist(const uint8_t* counts, const std::array<const Multiset*, RackCount>& sets, TestDetail& d);
    const ResistRec& resistRec(ResistTable& t, int set);
    ResistOrder& resistOrder(const uint8_t* counts, const std::array<int, RackCount>& free);
    bool resistAt(ResistOrder& o, size_t i, int& set);  // the i-th set of the order; false past the end
    enum class OrderStep
    {
        Emitted,
        Evaluate,
        Wait,
        Done
    };
    OrderStep stepOrder(ResistOrder& o, int& set, float& bound);
    double minD(const uint8_t* counts, const std::array<int, RackCount>& free);
    std::array<double, 3> fitSlack(const uint8_t* counts, const std::array<int, RackCount>& free);
    double feasibleMinD(const uint8_t* counts, const std::array<int, RackCount>& free, const CoreEval& ce,
                        const std::array<double, 3>& slack);

    // fitting side
    void buildFittingSets();
    struct FitRecord
    {
        std::array<float, 3> outputs{}, cost{};
    };
    const FitRecord& fitRecord(const uint8_t* counts, int fset);
    int findFitting(const uint8_t* counts, const std::array<int, RackCount>& free, double cpu, double power,
                    double calibration);

    // ---- stage 1
    void stage1();
    double partialBound(const uint8_t* fixed, unsigned freeMask);
    bool partialFits(const uint8_t* fixed, unsigned freeMask);
    bool fixedFits(const uint8_t* fixed, unsigned freeMask, CoreEval& scratch);
    std::array<double, 3> outputMax_{};  // most CPU / power / calibration any set can reach
    bool noFitPrune_ = [] {
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, "OPT_NO_FITPRUNE") == 0 && e;
        free(e);
        return on;
    }();
    double costScale_ = 1;               // most any rigs can scale module costs down
    bool noRigCalSplit_ = [] {           // OPT_NO_RIG_CAL_SPLIT: one rig subgroup per family (the old way)
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, "OPT_NO_RIG_CAL_SPLIT") == 0 && e;
        free(e);
        return on;
    }();
    void countsOf(const std::array<uint32_t, RackCount>& ms, int level, std::vector<uint8_t>& counts) const;
    // ---- stage 2
    void stage2();
    struct FinalSet
    {
        Scored s;
        std::vector<int> cands;  // cands_ indices
        FitInput fit;
    };
    std::vector<FinalSet> finals_;  // stage 2 (then refine) end sets, distinct
    void report();                  // the best fitting finals_ -> result_.fits

    // ---- refine: exact local search over every member (and charge) of every rack,
    // across subgroups and roles, from stage 2's sets; then iterated local search
    // within the time budget (see the comments at the definitions).
    std::vector<Candidate> chargeAlts_;             // non-best charges (measure)
    std::array<std::vector<int>, RackCount> choices_;     // module choices per rack (cands_ indices), dominated ones dropped
    std::array<std::vector<int>, RackCount> allChoices_;  // every module choice (memberBrute's reference)
    void buildPool();
    struct RSet
    {
        std::array<std::vector<int>, RackCount> m;  // sorted cands_ indices per rack
        Scored s;                                   // canonical evaluation
        // At a local optimum: the strongest one-move neighbours that don't improve it
        // (mostly upgrades that don't fit), (rack, out, in), strongest first; perturb() kicks with them.
        std::vector<std::array<int, 3>> kicks;
    };
    std::string keyOf(const RSet& x) const;
    bool validIds(const std::vector<int>& ids) const;
    bool validSet(const RSet& x) const;
    FitInput fitOf(const RSet& x) const;
    double priceOf(const RSet& x) const;
    Scored canonical(RSet& x);
    struct RefineStats
    {
        size_t sweeps = 0, pairChecks = 0, evaluations = 0, canonicalEvals = 0, plans = 0;
        double planSeconds = 0, evalSeconds = 0, canonicalSeconds = 0;  // thread time
    };
    bool improveOnce(RSet& x, RefineStats& st, bool pairs);
    void localSearch(RSet& x, RefineStats& st, const Clock::time_point* deadline);
    void refine(Clock::time_point t0);
    std::mutex canonM_;
    std::unordered_map<std::string, Scored> canonMemo_;
    std::atomic<size_t> refineEvals_{0};

    // ---- every combination of the given choices per rack, exactly (refine's exact
    // pass on small requests; Options::memberBruteForce with every choice)
    struct Enumerated
    {
        bool ran = false;
        std::string skipped;
        double combinations = 0, plans = 0, seconds = 0;
        size_t fitting = 0;
        Scored best;           // the best fitting set (goal, then cheaper)
        std::vector<int> ids;  // its modules
    };
    Enumerated enumerate(const std::array<std::vector<int>, RackCount>& lists, double limit,
                         double maxPlans = std::numeric_limits<double>::infinity());
    void memberBrute();

    // ---- validation (Options::bruteForce)
    void bruteForce();
    std::unordered_map<int, int> groupCaps() const;
    std::unique_ptr<Pool> pool_;

    // ---- helpers
    void logLine(const std::string& s)
    {
        if (log_)
            *log_ << s << "\n";
    }
    std::string candName(int c) const
    {
        const Candidate& cd = cands_[static_cast<size_t>(c)];
        std::string n = cd.type->name;
        if (cd.input.chargeID)
            if (const Type* ch = gd_.type(cd.input.chargeID))
                n += " [" + ch->name + "]";
        return n;
    }
    const std::vector<std::vector<int>>& projections(const std::vector<int>& kinds, unsigned freeMask);

    const GameData& gd_;
    const Engine& engine_;
    const Options& opt_;
    FitInput fit_;
    std::ostream* log_;
    Goal goal_;
    int layer_;
    bool active_;
    Result result_;

    FitInput base_;  // the pinned part
    std::array<int, RackCount> slots_{};
    std::unordered_map<int, int> pinnedGroupCount_, pinnedTypeCount_;
    const Type* ship_ = nullptr;

    std::vector<Candidate> cands_;
    std::vector<Subgroup> subgroups_;

    // kinds (one per subgroup)
    struct Kind
    {
        int subgroup = -1;
        int cand = -1;  // representative
        int rack = 0;
        Role role = Role::Core;
        int local = 0;  // index in its rack's list for its role
        int cap = 8;    // most copies allowed (group / type limits)
        int groupID = 0;
        bool injector = false;
    };
    std::vector<Kind> kinds_;
    std::array<std::vector<int>, RackCount> coreKinds_, resistKinds_, fitKinds_;

    // core plan and influence lists (kind indices)
    std::unique_ptr<SearchPlan> corePlan_;
    std::array<int, RackCount> coreFirstSlot_{};
    int coreSlots_ = 0;
    std::vector<int> coreOptionKind_;  // core plan global option -> kind
    std::vector<std::vector<int>> inflKind_;    // per kind
    std::vector<std::vector<int>> inflPinned_;  // per pinned module
    std::vector<int> inflCap_, inflHp_, eps_, inflFit_, capAll_;
    std::vector<ModuleStats> basePinned_;
    std::vector<bool> pinnedLive_;
    float basePeak_ = 0, baseHp_ = 0;
    std::vector<std::unique_ptr<Cache<ModuleStats>>> kindRec_, pinRec_;
    Cache<float> capRec_, hpRec_;
    Cache<double> budgetRec_;

    // resist sets
    std::array<std::vector<Multiset>, RackCount> resistMs_;
    struct SetRef
    {
        std::array<uint32_t, RackCount> ms{};
        std::array<uint8_t, RackCount> size{};
    };
    std::vector<SetRef> resistSets_;
    std::unique_ptr<SearchPlan> resistPlan_;
    std::array<int, RackCount> resistFirstSlot_{};
    std::vector<int> resistOptionKind_;
    std::array<std::vector<int>, RackCount> resistRackOptions_;  // kinds offered per rack in the resist plan
    std::vector<int> kindRack_;
    std::array<std::vector<size_t>, RackCount> resistSkip_;  // per resist multiset: the next one not extending it
    std::vector<bool> resistAdaptive_;  // per kind: resist kinds whose effect depends on other resist modules (RAH)
    Cache<std::shared_ptr<ResistTableSlot>> resistTables_;
    Cache<std::shared_ptr<ResistOrder>> resistOrders_;
    std::atomic<size_t> resistTableCount_{0}, resistExactTables_{0}, resistEvals_{0}, resistViolations_{0};
    std::atomic<int64_t> resistBoundNs_{0}, resistSortNs_{0};  // thread time building tables
    std::atomic<size_t> calls_{0};  // engine calls (Stats::engineCalls)
    bool resistVerify_ = false;     // evaluate every set at table build and check the bounds
    bool noAnalytic_ = false;       // OPT_NO_ANALYTIC: exact tables only (the old behaviour)

    // fitting sets
    std::array<std::vector<Multiset>, RackCount> fitMs_;
    std::vector<SetRef> fitSets_;  // ascending total size
    std::unique_ptr<SearchPlan> fitPlan_;
    std::array<int, RackCount> fitFirstSlot_{};
    std::vector<int> fitOptionKind_;
    std::array<std::vector<int>, RackCount> fitRackOptions_;
    Cache<FitRecord> fitRecs_;
    Cache<std::array<double, 3>> fitSlack_;

    // stage 1
    std::vector<int> rackOrder_;  // racks with core choices, in the order stage 1 fixes them
    std::array<std::vector<Multiset>, RackCount> coreMs_;
    Cache<std::vector<std::vector<int>>> projCache_;
    struct Found
    {
        double score;
        std::array<uint32_t, RackCount> ms;  // core multiset per rack
        int rset, fset;
        bool operator<(const Found& o) const { return score > o.score; }  // min-heap by score
    };
    std::vector<Found> top_;
    double maxModelError_ = 0;
};
}  // namespace

namespace
{
std::optional<int> rackOf(const GameData& gd, int typeID)
{
    const Type* t = gd.type(typeID);
    auto slot = t ? gd.moduleSlot(*t) : std::nullopt;
    if (!slot)
        return std::nullopt;
    for (int r = 0; r < RackCount; ++r)
        if (*slot == kRackSlot[r])
            return r;
    return std::nullopt;
}

void Search::setup()
{
    ship_ = gd_.type(fit_.shipTypeID);
    if (!ship_)
        throw std::invalid_argument("unknown ship");
    if (opt_.partial && opt_.openModules.size() != fit_.modules.size())
        throw std::invalid_argument("Options::openModules needs one flag per module");
    base_ = fit_;
    base_.modules.clear();
    std::array<int, RackCount> requested = opt_.partial ? opt_.openEmpty : std::array<int, RackCount>{};
    std::array<int, RackCount> pinnedInRack{};
    for (size_t i = 0; i < fit_.modules.size(); ++i)
    {
        const ModuleInput& m = fit_.modules[i];
        auto rack = rackOf(gd_, m.typeID);
        if (rack)
        {
            const size_t ri = static_cast<size_t>(*rack);
            if (opt_.partial ? opt_.openModules[i] != 0 : opt_.search[ri])
            {
                ++requested[ri];
                continue;
            }
            ++pinnedInRack[ri];
        }
        if (const Type* t = gd_.type(m.typeID))
        {
            // Module rules: the start fit is evaluated with the lowered state too.
            State st = clampPinnedState(gd_, *t, m.state);
            if (st != m.state)
            {
                logLine(std::string("pinned ") + t->name + " evaluated " + (st == State::Online ? "online" : "offline") +
                        " (module rule)");
                fit_.modules[i].state = st;
                result_.clamped.emplace_back(static_cast<int>(i), st);
            }
        }
        base_.modules.push_back(m);
        pinnedIndex_.push_back(static_cast<int>(i));
        if (const Type* t = gd_.type(m.typeID))
        {
            ++pinnedGroupCount_[t->groupID];
            ++pinnedTypeCount_[t->id];
        }
    }
    FitResult b = engine_.evaluate(base_);
    {
        const TestResult tb = toTestResult(b, goal_);
        pinnedUsed_ = {tb.cpuUsed, tb.powerUsed, tb.calibrationUsed};
        pinnedOutput_ = {tb.cpuOutput, tb.powerOutput, tb.calibrationOutput};
    }
    const Attr slotAttr[RackCount] = {Attr::medSlots, Attr::lowSlots, Attr::rigSlots};
    for (int r = 0; r < RackCount; ++r)
    {
        const size_t ri = static_cast<size_t>(r);
        const int rackSize = static_cast<int>(std::lround(b.shipAttr(slotAttr[r])));
        const int limit = std::clamp(opt_.maxSlots[ri], 0, kMaxRackSlots);
        if (opt_.partial)
            slots_[ri] = std::clamp(std::min(requested[ri], rackSize - pinnedInRack[ri]), 0, limit);
        else
            slots_[ri] = opt_.search[ri] ? std::clamp(rackSize, 0, limit) : 0;
    }
    result_.openSlots = slots_;
    result_.start = toTestResult(engine_.evaluate(fit_), goal_);
    logLine(std::string("ship ") + ship_->name + ": searching " + std::to_string(slots_[RackMid]) + " mid / " +
            std::to_string(slots_[RackLow]) + " low / " + std::to_string(slots_[RackRig]) + " rig slots, " +
            std::to_string(base_.modules.size()) + " pinned modules");
}

void Search::gatherCandidates()
{
    auto attrIDs = [&](const std::string& prefix) {
        std::vector<int> ids;
        for (const auto& [id, a] : gd_.allAttributes())
            if (a.name.rfind(prefix, 0) == 0)
                ids.push_back(id);
        return ids;
    };
    const std::vector<int> fitGroupAttrs = attrIDs("canFitShipGroup");
    std::vector<int> fitTypeAttrs = attrIDs("canFitShipType");
    if (int id = gd_.attributeID("fitsToShipType"))
        fitTypeAttrs.push_back(id);
    const int aVolume = gd_.attributeID("volume"), aRigSize = gd_.attributeID("rigSize"),
              aIsCapital = gd_.attributeID("isCapitalSize"), aMeta = gd_.attributeID("metaGroupID"),
              aMaxGroup = gd_.attributeID("maxGroupFitted"), aMaxType = gd_.attributeID("maxTypeFitted"),
              aCapacity = gd_.attributeID("capacity"), aChargeSize = gd_.attributeID("chargeSize");
    std::vector<int> chargeGroupAttrs;
    for (int i = 1; i <= 5; ++i)
        if (int id = gd_.attributeID("chargeGroup" + std::to_string(i)))
            chargeGroupAttrs.push_back(id);

    const bool shipCapital = ship_->attr(aIsCapital).value_or(0) == 1;
    const float shipRigSize = ship_->attr(aRigSize).value_or(0);

    size_t ruleExcluded = 0;
    for (int r = 0; r < RackCount; ++r)
    {
        if (!slots_[static_cast<size_t>(r)])
            continue;
        for (int id : gd_.moduleTypeIDs(kRackSlot[r]))
        {
            const Type* t = gd_.type(id);
            const Category* cat = gd_.categoryOf(*t);
            if (!cat || cat->name == "Structure Module")
                continue;
            bool restricted = false, allowed = false;
            for (int a : fitGroupAttrs)
                if (auto v = t->attr(a))
                {
                    restricted = true;
                    allowed |= static_cast<int>(*v) == ship_->groupID;
                }
            for (int a : fitTypeAttrs)
                if (auto v = t->attr(a))
                {
                    restricted = true;
                    allowed |= static_cast<int>(*v) == ship_->id;
                }
            if (restricted && !allowed)
                continue;
            if (!shipCapital && t->attr(aVolume).value_or(0) >= 4000)
                continue;
            if (r == RackRig && t->attr(aRigSize).value_or(0) != shipRigSize)
                continue;
            if (!opt_.allowOfficer && (t->metaGroupID == 5 || t->attr(aMeta).value_or(0) == 5))
                continue;
            const Group* g = gd_.groupOf(*t);
            if (goal_ == Goal::ShieldActive && g && g->name == "Capacitor Power Relay")
                continue;
            if (excludedFromSearch(gd_, *t))
            {
                ++ruleExcluded;
                continue;
            }
            int maxGroup = static_cast<int>(t->attr(aMaxGroup).value_or(0));
            int maxType = static_cast<int>(t->attr(aMaxType).value_or(0));
            if (maxGroup && pinnedGroupCount_[t->groupID] >= maxGroup)
                continue;
            if (maxType && pinnedTypeCount_[t->id] >= maxType)
                continue;
            double price = std::numeric_limits<double>::infinity();
            bool hasPrice = false;
            if (opt_.prices)
                if (auto p = opt_.prices->find(id); p != opt_.prices->end() && p->second > 0)
                    price = p->second, hasPrice = true;
            if (opt_.priceCap > 0 && (!hasPrice || price > opt_.priceCap))
                continue;
            Engine::Reach reach = engine_.reach(id);
            if (!reach.ship && !reach.others && !reach.special)
                continue;

            Candidate c;
            c.type = t;
            c.rack = r;
            c.groupID = t->groupID;
            c.maxGroup = maxGroup;
            c.maxType = maxType;
            c.price = opt_.prices ? price : 0;
            c.hasPrice = hasPrice;
            c.reach = reach;
            c.input.typeID = id;
            c.input.state = reach.activatable ? State::Active : State::Online;

            // Modules that only work with a charge: one candidate per valid charge,
            // the best one is kept after measuring.
            bool needsCharge = g && (g->name == "Ancillary Shield Booster" || g->name == "Ancillary Armor Repairer" ||
                                     g->name == "Capacitor Booster");
            if (!needsCharge)
            {
                cands_.push_back(c);
                continue;
            }
            float capacity = t->attr(aCapacity).value_or(0);
            float size = t->attr(aChargeSize).value_or(0);
            for (int ga : chargeGroupAttrs)
            {
                auto cg = t->attr(ga);
                if (!cg || *cg == 0)
                    continue;
                for (int chargeID : gd_.typeIDsInGroup(static_cast<int>(*cg)))
                {
                    const Type* ch = gd_.type(chargeID);
                    if (ch->attr(aVolume).value_or(0) > capacity)
                        continue;
                    if (size > 0 && ch->attr(aChargeSize).value_or(0) != size)
                        continue;
                    Candidate cc = c;
                    cc.input.chargeID = chargeID;
                    // Prices compare module + charge, as the response reports them (the cap
                    // applies to the module); an unknown charge price counts 0.
                    if (opt_.prices)
                        if (auto p = opt_.prices->find(chargeID); p != opt_.prices->end() && p->second > 0)
                            cc.price += p->second;
                    cands_.push_back(cc);
                }
            }
        }
    }
    logLine(std::to_string(cands_.size()) + " candidate modules/charges after static filters (" +
            std::to_string(ruleExcluded) + " excluded by module rules)");
}

void Search::measure()
{
    // Each candidate alone on the pinned fit: one plan per chunk, one slot.
    TestDetail d0, d;
    TestResult r0{};
    bool haveEmpty = false;
    uint64_t resBits = 0;
    for (const auto& layer : kResAttrs)
        for (Attr a : layer)
            resBits |= uint64_t{1} << shipField(a);
    const int budget = kPlanItemBudget - 2 * static_cast<int>(base_.modules.size()) - 4;
    for (size_t first = 0; first < cands_.size();)
    {
        SearchSpec spec;
        spec.base = base_;
        std::vector<ModuleInput> options;
        size_t last = first;
        int items = 0;
        while (last < cands_.size() && items + 2 <= budget)
        {
            options.push_back(cands_[last].input);
            items += cands_[last].input.chargeID ? 2 : 1;
            ++last;
        }
        spec.slots.push_back(std::move(options));
        SearchPlan plan(engine_, spec);
        if (!haveEmpty)
        {
            int none = -1;
            r0 = plan.evaluate(&none, goal_, &d0);
            haveEmpty = true;
        }
        for (size_t i = first; i < last; ++i)
        {
            int choice = static_cast<int>(i - first);
            TestResult r = plan.evaluate(&choice, goal_, &d);
            calls_.fetch_add(1, std::memory_order_relaxed);
            Candidate& c = cands_[i];
            c.goalGain = r.goal - r0.goal;
            c.dividerDrop = d0.divider[static_cast<size_t>(layer_)] - d.divider[static_cast<size_t>(layer_)];
            c.hpGain = d.hp[static_cast<size_t>(layer_)] - d0.hp[static_cast<size_t>(layer_)];
            c.capGain = (r.capRecharge - r.capUsed) - (r0.capRecharge - r0.capUsed);
            const float outs[3] = {r.cpuOutput, r.powerOutput, r.calibrationOutput};
            const float outs0[3] = {r0.cpuOutput, r0.powerOutput, r0.calibrationOutput};
            c.outputGain = 0;
            for (int k = 0; k < 3; ++k)
            {
                c.outputDelta[static_cast<size_t>(k)] = outs[k] - outs0[k];
                if (outs0[k] > 0)
                    c.outputGain += std::max(0.0, static_cast<double>(outs[k] - outs0[k]) / outs0[k]);
            }
            const ModuleStats& m = d.modules[static_cast<size_t>(d.moduleCount - 1)];
            c.cpu = m.cpu;
            c.power = m.power;
            c.calibration = m.calibration;
            {
                const double used[3] = {r.cpuUsed, r.powerUsed, r.calibrationUsed};
                const double used0[3] = {r0.cpuUsed, r0.powerUsed, r0.calibrationUsed};
                const double own[3] = {m.cpu, m.power, m.calibration};
                for (size_t k = 0; k < 3; ++k)
                    c.pinnedDrop[k] = std::max(0.0, -((used[k] - used0[k]) - own[k]));
            }
            c.capUse = (m.state >= State::Active && m.capNeed != 0 && m.rawCycle + m.reactivation > 0) ? m.capUse : 0;
            c.rep = repStrength(m, layer_);
            c.shipChanged = 0;
            for (size_t f = 0; f < std::size(fitschema::ShipAttrs::kFields); ++f)
                if (std::abs(d.ship[f] - d0.ship[f]) > 1e-6f * std::max(std::abs(d0.ship[f]), 1e-6f))
                    c.shipChanged |= uint64_t{1} << f;
            c.resMask = 0;
            int bit = 0;
            for (size_t f = 0; f < std::size(fitschema::ShipAttrs::kFields); ++f)
                if (resBits >> f & 1)
                {
                    float a = d.ship[f], b = d0.ship[f];
                    if (std::abs(a - b) > 1e-6f * std::max(std::abs(b), 1e-6f))
                        c.resMask |= static_cast<uint16_t>(1u << bit);
                    ++bit;
                }
        }
        first = last;
    }

    // Keep each charge-needing module's best charge.
    std::vector<Candidate> kept;
    std::unordered_map<int, size_t> best;  // typeID -> index in kept
    auto better = [&](const Candidate& a, const Candidate& b) {
        if (a.rep != b.rep)
            return a.rep > b.rep;
        if (a.capGain != b.capGain)
            return a.capGain > b.capGain;
        return a.goalGain > b.goalGain;
    };
    // The other charges are kept aside: refine() offers them as module choices of their own.
    chargeAlts_.clear();
    for (Candidate& c : cands_)
    {
        if (!c.input.chargeID)
        {
            kept.push_back(std::move(c));
            continue;
        }
        auto it = best.find(c.input.typeID);
        if (it == best.end())
        {
            best[c.input.typeID] = kept.size();
            kept.push_back(std::move(c));
        }
        else if (better(c, kept[it->second]))
        {
            chargeAlts_.push_back(std::move(kept[it->second]));
            kept[it->second] = std::move(c);
        }
        else
            chargeAlts_.push_back(std::move(c));
    }
    cands_ = std::move(kept);
}
}  // namespace

namespace
{
bool directlyUseful(const Candidate& c, bool active)
{
    if (c.dividerDrop > 1e-9 || c.outputGain > kTiny || c.goalGain > kTiny)
        return true;
    return active ? (c.rep > kTiny || c.capGain > kTiny) : c.hpGain > kTiny;
}

// Strongest first by what a module does on its own; ties to the cheaper one.
bool directlyStronger(const Candidate& x, const Candidate& y, bool active)
{
    const double a[4] = {x.rep + (active ? 0.0 : x.hpGain), active ? x.capGain : 0.0, x.dividerDrop, x.outputGain};
    const double b[4] = {y.rep + (active ? 0.0 : y.hpGain), active ? y.capGain : 0.0, y.dividerDrop, y.outputGain};
    for (int k = 0; k < 4; ++k)
        if (a[k] != b[k])
            return a[k] > b[k];
    return x.price < y.price;
}

void Search::interactions()
{
    // Subgroups: same rack, group (rigs: family), resonances changed and size.
    // Their members do the same thing at different strengths, so influence is
    // measured between representatives only.
    std::unordered_map<std::string, int> byKey;
    for (size_t i = 0; i < cands_.size(); ++i)
    {
        Candidate& c = cands_[i];
        if (!directlyUseful(c, active_) && !c.reach.others)
            continue;
        const Group* g = gd_.groupOf(*c.type);
        std::string key = std::string(rackName(c.rack)) + "|";
        if (c.rack == RackRig)
            // Split by calibration cost: T1 and T2 rigs of a family differ in it, and the
            // stronger (dearer) representative alone can make stage 1 miss sets where
            // only the cheaper rig fits (e.g. a CPU rig next to pinned rigs).
            key += rigFamily(c.type->name) +
                   (noRigCalSplit_ ? std::string() : "|cal" + std::to_string(std::lround(c.calibration)));
        else
            key += (g ? g->name : std::to_string(c.groupID)) + "|" + std::to_string(c.resMask) + "|" +
                   sizeToken(c.type->name);
        auto [it, inserted] = byKey.emplace(key, static_cast<int>(subgroups_.size()));
        if (inserted)
        {
            Subgroup sg;
            sg.rack = c.rack;
            sg.key = key;
            subgroups_.push_back(sg);
        }
        c.subgroup = it->second;
        subgroups_[static_cast<size_t>(it->second)].members.push_back(static_cast<int>(i));
    }
    for (Subgroup& sg : subgroups_)
        std::stable_sort(sg.members.begin(), sg.members.end(), [&](int a, int b) {
            return directlyStronger(cands_[static_cast<size_t>(a)], cands_[static_cast<size_t>(b)], active_);
        });

    // Influence between representatives.
    SearchSpec spec;
    spec.base = base_;
    spec.keepInfluence = true;
    std::vector<ModuleInput> options;
    for (const Subgroup& sg : subgroups_)
        options.push_back(cands_[static_cast<size_t>(sg.members.front())].input);
    spec.slots.push_back(std::move(options));
    SearchPlan plan(engine_, spec);
    for (size_t s = 0; s < subgroups_.size(); ++s)
    {
        Influence inf = plan.influence(0, static_cast<int>(s));  // option index = subgroup index
        subgroups_[s].influence = std::move(inf);
    }

    // Pair gains: what each member of an influencing subgroup adds to the
    // representative of every useful subgroup it modifies.
    std::vector<int> influencers;  // candidates
    std::set<int> targetSet;       // subgroups
    for (size_t s = 0; s < subgroups_.size(); ++s)
    {
        bool any = false;
        for (const auto& [t, attr] : subgroups_[s].influence.optionFields)
            if (t != static_cast<int>(s) &&
                directlyUseful(cands_[static_cast<size_t>(subgroups_[static_cast<size_t>(t)].members.front())], active_))
            {
                targetSet.insert(t);
                any = true;
            }
        if (any)
            for (int m : subgroups_[s].members)
                influencers.push_back(m);
    }
    if (influencers.empty())
        return;
    std::vector<int> targets(targetSet.begin(), targetSet.end());
    const int budget = kPlanItemBudget - 2 * static_cast<int>(base_.modules.size()) - 4;
    TestDetail dk, dt;
    // Chunks of influencers x all targets.
    int targetItems = 0;
    for (int t : targets)
        targetItems += cands_[static_cast<size_t>(subgroups_[static_cast<size_t>(t)].members.front())].input.chargeID ? 2 : 1;
    const int inflBudget = std::max(16, budget - targetItems);
    for (size_t first = 0; first < influencers.size();)
    {
        SearchSpec ps;
        ps.base = base_;
        std::vector<ModuleInput> inflOptions, targetOptions;
        size_t last = first;
        int items = 0;
        while (last < influencers.size() && items + 2 <= inflBudget)
        {
            inflOptions.push_back(cands_[static_cast<size_t>(influencers[last])].input);
            items += cands_[static_cast<size_t>(influencers[last])].input.chargeID ? 2 : 1;
            ++last;
        }
        for (int t : targets)
            targetOptions.push_back(cands_[static_cast<size_t>(subgroups_[static_cast<size_t>(t)].members.front())].input);
        ps.slots = {inflOptions, targetOptions};
        SearchPlan pp(engine_, ps);
        for (size_t ti = 0; ti < targets.size(); ++ti)
        {
            int t = targets[ti];
            int choice[2] = {-1, static_cast<int>(ti)};
            TestResult rt = pp.evaluate(choice, goal_, &dt);
            calls_.fetch_add(1, std::memory_order_relaxed);
            const ModuleStats mt = dt.modules[static_cast<size_t>(dt.moduleCount - 1)];
            for (size_t ki = first; ki < last; ++ki)
            {
                Candidate& c = cands_[static_cast<size_t>(influencers[ki])];
                const auto& of = subgroups_[static_cast<size_t>(c.subgroup)].influence.optionFields;
                if (!std::any_of(of.begin(), of.end(), [&](const auto& p) { return p.first == t; }))
                    continue;
                choice[0] = static_cast<int>(ki - first);
                TestResult rk = pp.evaluate(choice, goal_, &dk);
                calls_.fetch_add(1, std::memory_order_relaxed);
                const ModuleStats& mk = dk.modules[static_cast<size_t>(dk.moduleCount - 1)];
                double gain = (rk.goal - rt.goal) + (repStrength(mk, layer_) - repStrength(mt, layer_));
                if (mt.state >= State::Active && mk.capUse > 0 && mt.capUse > 0)
                    gain += mt.capUse - mk.capUse;
                c.pairGain = std::max(c.pairGain, gain);
                double costAlone = mt.cpu + mt.power + mt.calibration;
                if (costAlone > 0)
                    c.costGain = std::max(c.costGain, (costAlone - (mk.cpu + mk.power + mk.calibration)) / costAlone);
            }
        }
        first = last;
    }
}

void Search::classify()
{
    uint64_t resBits = 0, outBits = 0;
    for (const auto& layer : kResAttrs)
        for (Attr a : layer)
            resBits |= uint64_t{1} << shipField(a);
    for (Attr a : kOutputAttrs)
        outBits |= uint64_t{1} << shipField(a);

    // Useful members only; empty subgroups go.
    for (Subgroup& sg : subgroups_)
    {
        std::vector<int> kept;
        for (int m : sg.members)
        {
            Candidate& c = cands_[static_cast<size_t>(m)];
            c.useful = directlyUseful(c, active_) || c.pairGain > kTiny || c.costGain > kTiny;
            if (c.useful)
                kept.push_back(m);
        }
        sg.members = std::move(kept);
    }

    // Roles. Resist modules change only resonances (measured alone) and only modify
    // other resist modules (the reactive hardener adapts to them): resist sets are
    // evaluated together, so that stays exact. Fitting modules change only outputs.
    const size_t n = subgroups_.size();
    std::vector<bool> resist(n, false);
    for (size_t s = 0; s < n; ++s)
    {
        const Subgroup& sg = subgroups_[s];
        if (sg.members.empty() || sg.rack == RackRig)
            continue;
        const Candidate& c = cands_[static_cast<size_t>(sg.members.front())];
        resist[s] = c.dividerDrop > 1e-9 && c.rep <= kTiny && c.hpGain <= kTiny && (c.shipChanged & ~resBits) == 0 &&
                    sg.influence.pinnedFields.empty();
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t s = 0; s < n; ++s)
        {
            if (!resist[s])
                continue;
            for (const auto& [t, attr] : subgroups_[s].influence.optionFields)
                if (t != static_cast<int>(s) && !subgroups_[static_cast<size_t>(t)].members.empty() &&
                    !resist[static_cast<size_t>(t)])
                {
                    resist[s] = false;
                    changed = true;
                    break;
                }
        }
    }
    for (size_t s = 0; s < n; ++s)
    {
        Subgroup& sg = subgroups_[s];
        if (sg.members.empty())
            continue;
        const Candidate& c = cands_[static_cast<size_t>(sg.members.front())];
        bool writesOthers = false;
        for (const auto& [t, attr] : sg.influence.optionFields)
            writesOthers |= t != static_cast<int>(s) && !subgroups_[static_cast<size_t>(t)].members.empty();
        if (resist[s])
            sg.role = Role::Resist;
        else if (sg.rack != RackRig && c.outputGain > kTiny && !writesOthers && sg.influence.pinnedFields.empty() &&
                 (c.shipChanged & ~outBits) == 0 && c.capUse <= 0)
            sg.role = Role::Fitting;
        else
            sg.role = Role::Core;
        sg.key = std::string(roleName(sg.role)) + "|" + sg.key;
        for (int m : sg.members)
        {
            Candidate& mc = cands_[static_cast<size_t>(m)];
            mc.role = sg.role;
            double s0 = mc.rep + (active_ ? 0.0 : mc.hpGain);
            double s1 = active_ ? mc.capGain : 0.0;
            switch (sg.role)
            {
            case Role::Core: mc.strength = {s0, mc.pairGain, s1, mc.dividerDrop, mc.outputGain, mc.costGain}; break;
            case Role::Resist: mc.strength = {mc.dividerDrop, s1, 0, 0, 0, 0}; break;
            case Role::Fitting: mc.strength = {mc.outputGain, 0, 0, 0, 0, 0}; break;
            }
        }
        std::stable_sort(sg.members.begin(), sg.members.end(), [&](int a, int b) {
            const Candidate& x = cands_[static_cast<size_t>(a)];
            const Candidate& y = cands_[static_cast<size_t>(b)];
            for (size_t k = 0; k < x.strength.size(); ++k)
                if (x.strength[k] != y.strength[k])
                    return x.strength[k] > y.strength[k];
            if (x.price != y.price)
                return sg.rack == RackRig ? x.price > y.price : x.price < y.price;
            return x.cpu + x.power < y.cpu + y.power;
        });
        result_.stats.candidates += sg.members.size();
    }

    if (opt_.dumpKinds && log_)
    {
        for (const Subgroup& sg : subgroups_)
        {
            if (sg.members.empty())
                continue;
            *log_ << "  [" << sg.key << "] " << sg.members.size() << " members | writes ship:";
            for (size_t f = 0; f < std::size(fitschema::ShipAttrs::kFields); ++f)
                if (sg.influence.shipFields >> f & 1)
                    *log_ << " " << fitschema::kAttrName[static_cast<size_t>(fitschema::ShipAttrs::kFields[f])];
            *log_ << "; modules:";
            std::set<int> seen;
            for (const auto& [t, a] : sg.influence.optionFields)
                if (seen.insert(t).second && !subgroups_[static_cast<size_t>(t)].members.empty())
                    *log_ << " [" << subgroups_[static_cast<size_t>(t)].key << "]";
            *log_ << "; pinned: " << sg.influence.pinnedFields.size() << "\n";
            for (size_t m = 0; m < sg.members.size(); ++m)
            {
                const Candidate& c = cands_[static_cast<size_t>(sg.members[m])];
                *log_ << "      " << (m == 0 ? "* " : "  ") << candName(sg.members[m]) << "  rep " << c.rep << " cap "
                      << c.capGain << " res " << c.dividerDrop << " hp " << c.hpGain << " out " << c.outputGain
                      << " pair " << c.pairGain << " cost " << c.costGain << " | cpu " << c.cpu << " pg " << c.power
                      << " cal " << c.calibration << " | " << (c.hasPrice ? c.price / 1e6 : -1) << "M\n";
            }
        }
    }
}
}  // namespace

namespace
{
void addUnique(std::vector<int>& v, int x)
{
    if (std::find(v.begin(), v.end(), x) == v.end())
        v.push_back(x);
}

void Search::buildKinds()
{
    for (size_t s = 0; s < subgroups_.size(); ++s)
    {
        const Subgroup& sg = subgroups_[s];
        if (sg.members.empty())
            continue;
        Kind k;
        k.subgroup = static_cast<int>(s);
        k.cand = sg.members.front();
        k.rack = sg.rack;
        k.role = sg.role;
        const Candidate& c = cands_[static_cast<size_t>(k.cand)];
        k.groupID = c.groupID;
        k.cap = slots_[static_cast<size_t>(k.rack)];
        if (c.maxGroup)
            k.cap = std::min(k.cap, c.maxGroup - pinnedGroupCount_[c.groupID]);
        if (c.maxType)
            k.cap = std::min(k.cap, c.maxType - pinnedTypeCount_[c.type->id]);
        k.injector = c.capUse < 0;
        auto& list = k.role == Role::Core ? coreKinds_[static_cast<size_t>(k.rack)]
                     : k.role == Role::Resist ? resistKinds_[static_cast<size_t>(k.rack)]
                                              : fitKinds_[static_cast<size_t>(k.rack)];
        k.local = static_cast<int>(list.size());
        list.push_back(static_cast<int>(kinds_.size()));
        kinds_.push_back(k);
        if (k.role == Role::Core)
            ++result_.stats.coreKinds;
        else if (k.role == Role::Resist)
            ++result_.stats.resistKinds;
        else
            ++result_.stats.fittingKinds;
    }
    result_.stats.kinds = kinds_.size();
    if (kinds_.size() > 250)
        throw ModelLimit("too many module kinds");
}

void Search::buildCorePlan()
{
    SearchSpec spec;
    spec.base = base_;
    spec.keepInfluence = true;
    int items = 0;
    for (int r = 0; r < RackCount; ++r)
    {
        coreFirstSlot_[static_cast<size_t>(r)] = static_cast<int>(spec.slots.size());
        const auto& list = coreKinds_[static_cast<size_t>(r)];
        if (list.empty())
            continue;
        std::vector<ModuleInput> options;
        for (int k : list)
        {
            const Candidate& c = cands_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].cand)];
            options.push_back(c.input);
            items += c.input.chargeID ? 2 : 1;
        }
        for (int s = 0; s < slots_[static_cast<size_t>(r)]; ++s)
            spec.slots.push_back(options);
    }
    if (items * kMaxRackSlots > 4 * kPlanItemBudget)
        throw ModelLimit("too many core kinds");
    coreSlots_ = static_cast<int>(spec.slots.size());
    corePlan_ = std::make_unique<SearchPlan>(engine_, spec);
    coreOptionKind_.assign(static_cast<size_t>(corePlan_->optionCount()), -1);
    for (int r = 0; r < RackCount; ++r)
    {
        const auto& list = coreKinds_[static_cast<size_t>(r)];
        if (list.empty())
            continue;
        for (int s = 0; s < slots_[static_cast<size_t>(r)]; ++s)
            for (size_t l = 0; l < list.size(); ++l)
                coreOptionKind_[static_cast<size_t>(
                    corePlan_->optionIndex(coreFirstSlot_[static_cast<size_t>(r)] + s, static_cast<int>(l)))] =
                    list[l];
    }

    // Base values (nothing chosen).
    std::vector<int> none(spec.slots.size(), -1);
    TestDetail d;
    corePlan_->evaluate(none.data(), goal_, &d);
    basePinned_.assign(base_.modules.size(), ModuleStats{});
    pinnedLive_.assign(base_.modules.size(), false);
    for (int i = 0; i < d.moduleCount; ++i)
    {
        int p = -1 - d.moduleOption[static_cast<size_t>(i)];
        if (p >= 0 && p < static_cast<int>(base_.modules.size()))
        {
            basePinned_[static_cast<size_t>(p)] = d.modules[static_cast<size_t>(i)];
            pinnedLive_[static_cast<size_t>(p)] = true;
        }
    }
    basePeak_ = d.capPeak;
    baseHp_ = d.hp[static_cast<size_t>(layer_)];

    // Influence among core kinds (incl. copies of the same kind), on pinned modules and on the ship.
    const uint64_t capBits = shipMask({Attr::capacitorCapacity, Attr::rechargeRate});
    const uint64_t hpBits = shipMask({kHpAttr[layer_]});
    const uint64_t resBits = shipMask({kResAttrs[layer_][0], kResAttrs[layer_][1], kResAttrs[layer_][2],
                                       kResAttrs[layer_][3]});
    const uint64_t outBits = shipMask({kOutputAttrs[0], kOutputAttrs[1], kOutputAttrs[2]});
    inflKind_.assign(kinds_.size(), {});
    inflPinned_.assign(base_.modules.size(), {});
    for (int r = 0; r < RackCount; ++r)
        for (int k : coreKinds_[static_cast<size_t>(r)])
        {
            const Kind& kind = kinds_[static_cast<size_t>(k)];
            Influence inf = corePlan_->influence(coreFirstSlot_[static_cast<size_t>(r)], kind.local);
            for (const auto& [opt, attr] : inf.optionFields)
                if (int j = coreOptionKind_[static_cast<size_t>(opt)]; j >= 0)
                    addUnique(inflKind_[static_cast<size_t>(j)], k);
            for (const auto& [p, attr] : inf.pinnedFields)
                if (p >= 0 && p < static_cast<int>(inflPinned_.size()))
                    addUnique(inflPinned_[static_cast<size_t>(p)], k);
            if (inf.shipFields & capBits)
                addUnique(inflCap_, k);
            if (inf.shipFields & hpBits)
                addUnique(inflHp_, k);
            if (inf.shipFields & resBits)
                addUnique(eps_, k);
            if (inf.shipFields & outBits)
                addUnique(inflFit_, k);
            // Effects on resist and fitting modules come from the interaction plan.
            for (const auto& [t, attr] : subgroups_[static_cast<size_t>(kind.subgroup)].influence.optionFields)
            {
                const Subgroup& target = subgroups_[static_cast<size_t>(t)];
                if (target.members.empty())
                    continue;
                Role role = target.role;
                if (role == Role::Resist)
                    addUnique(eps_, k);
                else if (role == Role::Fitting)
                    addUnique(inflFit_, k);
            }
        }
    // Kinds that decide the capacitor budget (without repairers).
    capAll_ = inflCap_;
    for (size_t k = 0; k < kinds_.size(); ++k)
        if (kinds_[k].role == Role::Core && kinds_[k].injector)
        {
            addUnique(capAll_, static_cast<int>(k));
            for (int j : inflKind_[k])
                addUnique(capAll_, j);
        }
    for (size_t p = 0; p < basePinned_.size(); ++p)
    {
        const ModuleStats& m = basePinned_[p];
        if (pinnedLive_[p] && !m.repEffects && m.capUse != 0)
            for (int j : inflPinned_[p])
                addUnique(capAll_, j);
    }
    for (auto* v : {&inflCap_, &inflHp_, &eps_, &inflFit_, &capAll_})
        std::sort(v->begin(), v->end());
    for (auto& v : inflKind_)
        std::sort(v.begin(), v.end());
    for (auto& v : inflPinned_)
        std::sort(v.begin(), v.end());
    for (auto* v : {&inflCap_, &inflHp_, &eps_, &inflFit_, &capAll_})
        if (v->size() > 32)
            throw ModelLimit("too many influencing kinds for one record");
    kindRec_.clear();
    pinRec_.clear();
    for (size_t k = 0; k < kinds_.size(); ++k)
        kindRec_.push_back(std::make_unique<Cache<ModuleStats>>());
    for (size_t p = 0; p < base_.modules.size(); ++p)
        pinRec_.push_back(std::make_unique<Cache<ModuleStats>>());
    if (log_)
    {
        auto names = [&](const std::vector<int>& v) {
            std::string s;
            for (int k : v)
                s += (s.empty() ? "" : ", ") + cands_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].cand)].type->name;
            return s.empty() ? std::string("-") : s;
        };
        *log_ << "core plan: " << corePlan_->stats().items << " items, " << corePlan_->stats().dynamic
              << " replayed instructions\n";
        *log_ << "  capacitor: " << names(inflCap_) << "\n  hp: " << names(inflHp_) << "\n  resist extras: "
              << names(eps_) << "\n  fitting: " << names(inflFit_) << "\n";
    }
}

void Search::evaluateCore(const uint8_t* projection, TestDetail& d)
{
    std::vector<int> choice(static_cast<size_t>(coreSlots_), -1);
    for (int r = 0; r < RackCount; ++r)
    {
        int slot = coreFirstSlot_[static_cast<size_t>(r)];
        for (int k : coreKinds_[static_cast<size_t>(r)])
            for (int c = 0; c < projection[k]; ++c)
                choice[static_cast<size_t>(slot++)] = kinds_[static_cast<size_t>(k)].local;
    }
    corePlan_->evaluate(choice.data(), goal_, &d);
    calls_.fetch_add(1, std::memory_order_relaxed);
}

ModuleStats Search::kindRecord(int kind, const uint8_t* counts)
{
    const auto& infl = inflKind_[static_cast<size_t>(kind)];
    Key key = packCounts(infl, counts);
    Cache<ModuleStats>& cache = *kindRec_[static_cast<size_t>(kind)];
    return cache.get(key, [&] {
        std::vector<uint8_t> proj(kinds_.size(), 0);
        for (int j : infl)
            proj[static_cast<size_t>(j)] = counts[j];
        proj[static_cast<size_t>(kind)] = std::max<uint8_t>(proj[static_cast<size_t>(kind)], 1);
        static thread_local TestDetail d;
        evaluateCore(proj.data(), d);
        ModuleStats out{};
        for (int i = 0; i < d.moduleCount; ++i)
        {
            int opt = d.moduleOption[static_cast<size_t>(i)];
            if (opt >= 0 && coreOptionKind_[static_cast<size_t>(opt)] == kind)
            {
                out = d.modules[static_cast<size_t>(i)];
                break;
            }
        }
        return out;
    });
}

ModuleStats Search::pinnedRecord(int pinned, const uint8_t* counts)
{
    const auto& infl = inflPinned_[static_cast<size_t>(pinned)];
    Key key = packCounts(infl, counts);
    Cache<ModuleStats>& cache = *pinRec_[static_cast<size_t>(pinned)];
    return cache.get(key, [&] {
        std::vector<uint8_t> proj(kinds_.size(), 0);
        for (int j : infl)
            proj[static_cast<size_t>(j)] = counts[j];
        static thread_local TestDetail d;
        evaluateCore(proj.data(), d);
        ModuleStats out = basePinned_[static_cast<size_t>(pinned)];
        for (int i = 0; i < d.moduleCount; ++i)
            if (d.moduleOption[static_cast<size_t>(i)] == -1 - pinned)
                out = d.modules[static_cast<size_t>(i)];
        return out;
    });
}

float Search::capPeakRecord(const uint8_t* counts)
{
    Key key = packCounts(inflCap_, counts);
    return capRec_.get(key, [&] {
        std::vector<uint8_t> proj(kinds_.size(), 0);
        for (int j : inflCap_)
            proj[static_cast<size_t>(j)] = counts[j];
        static thread_local TestDetail d;
        evaluateCore(proj.data(), d);
        return d.capPeak;
    });
}

float Search::hpRecord(const uint8_t* counts)
{
    Key key = packCounts(inflHp_, counts);
    return hpRec_.get(key, [&] {
        std::vector<uint8_t> proj(kinds_.size(), 0);
        for (int j : inflHp_)
            proj[static_cast<size_t>(j)] = counts[j];
        static thread_local TestDetail d;
        evaluateCore(proj.data(), d);
        float hp = d.hp[static_cast<size_t>(layer_)];
        return hp;
    });
}

// Capacitor left for repairers (recharge + boosters - other modules' use) with only
// the capacitor-relevant kinds of a core. Composed from the module and capacitor
// records like assemble() (no engine call of its own): the same arithmetic as the
// core values it bounds, and far fewer distinct keys to measure.
double Search::budgetRecord(const uint8_t* counts)
{
    Key key = packCounts(capAll_, counts);
    return budgetRec_.get(key, [&] {
        static thread_local std::vector<uint8_t> proj;
        static thread_local std::vector<ModuleStats> stats;
        proj.assign(kinds_.size(), 0);
        for (int j : capAll_)
            proj[static_cast<size_t>(j)] = counts[j];
        stats.clear();
        for (size_t p = 0; p < basePinned_.size(); ++p)
            if (pinnedLive_[p])
                stats.push_back(inflPinned_[p].empty() ? basePinned_[p] : pinnedRecord(static_cast<int>(p), proj.data()));
        for (int r = 0; r < RackCount; ++r)
            for (int k : coreKinds_[static_cast<size_t>(r)])
                if (int c = proj[static_cast<size_t>(k)])
                {
                    ModuleStats s = kindRecord(k, proj.data());
                    for (int i = 0; i < c; ++i)
                        stats.push_back(s);
                }
        float peak = inflCap_.empty() ? basePeak_ : capPeakRecord(proj.data());
        CapSums cs = capSums(stats.data(), static_cast<int>(stats.size()));
        RepProfile p = makeRepProfile(stats.data(), static_cast<int>(stats.size()), cs.used, cs.added + peak);
        return p.capRecharge - p.capUsed;
    });
}

void Search::assemble(const uint8_t* counts, CoreEval& out)
{
    static thread_local std::vector<ModuleStats> stats;
    stats.clear();
    for (size_t p = 0; p < basePinned_.size(); ++p)
    {
        if (!pinnedLive_[p])
            continue;
        stats.push_back(inflPinned_[p].empty() ? basePinned_[p] : pinnedRecord(static_cast<int>(p), counts));
    }
    for (int r = 0; r < RackCount; ++r)
        for (int k : coreKinds_[static_cast<size_t>(r)])
            if (int c = counts[k])
            {
                ModuleStats s = kindRecord(k, counts);
                for (int i = 0; i < c; ++i)
                    stats.push_back(s);
            }
    out.cpu = out.power = out.calibration = 0;
    for (const ModuleStats& s : stats)
    {
        out.cpu += s.cpu;
        out.power += s.power;
        out.calibration += s.calibration;
    }
    if (active_)
    {
        float peak = inflCap_.empty() ? basePeak_ : capPeakRecord(counts);
        CapSums cs = capSums(stats.data(), static_cast<int>(stats.size()));
        out.profile = makeRepProfile(stats.data(), static_cast<int>(stats.size()), cs.used, cs.added + peak);
        out.v0 = out.profile.repair(layer_, 0);
    }
    else
        out.v0 = inflHp_.empty() ? baseHp_ : hpRecord(counts);
}
}  // namespace

namespace
{
// Multisets of a rack's kinds with per-kind caps and shared group limits.
std::vector<Multiset> rackMultisets(const std::vector<int>& list, int maxSize, const std::vector<int>& kindCap,
                                    const std::vector<int>& kindGroup, const std::unordered_map<int, int>& groupCap)
{
    std::vector<Multiset> out;
    Multiset cur;
    std::vector<int> used(list.size(), 0);
    std::unordered_map<int, int> groupUsed;
    std::function<void(int)> rec = [&](int from) {
        out.push_back(cur);
        if (cur.n >= maxSize)
            return;
        for (int i = from; i < static_cast<int>(list.size()); ++i)
        {
            if (used[static_cast<size_t>(i)] >= kindCap[static_cast<size_t>(i)])
                continue;
            int g = kindGroup[static_cast<size_t>(i)];
            auto gc = groupCap.find(g);
            if (gc != groupCap.end() && groupUsed[g] >= gc->second)
                continue;
            ++used[static_cast<size_t>(i)];
            ++groupUsed[g];
            cur.k[cur.n++] = static_cast<uint8_t>(i);
            rec(i);
            --cur.n;
            --groupUsed[g];
            --used[static_cast<size_t>(i)];
        }
    };
    rec(0);
    return out;
}

uint32_t freeCode(const std::array<int, RackCount>& free)
{
    return static_cast<uint32_t>(free[0] | free[1] << 4 | free[2] << 8);
}

// Group limits (maxGroupFitted of the representatives) left after the pinned modules.
std::unordered_map<int, int> Search::groupCaps() const
{
    std::unordered_map<int, int> groupCap;
    for (const Kind& k : kinds_)
    {
        const Candidate& c = cands_[static_cast<size_t>(k.cand)];
        if (c.maxGroup)
        {
            auto p = pinnedGroupCount_.find(k.groupID);
            groupCap[k.groupID] = c.maxGroup - (p == pinnedGroupCount_.end() ? 0 : p->second);
        }
    }
    return groupCap;
}

void Search::buildResistSets()
{
    const std::unordered_map<int, int> groupCap = groupCaps();
    kindRack_.resize(kinds_.size());
    for (size_t k = 0; k < kinds_.size(); ++k)
        kindRack_[k] = kinds_[k].rack;
    // Resist kinds another resist kind modifies (the reactive hardener reads the resonances).
    resistAdaptive_.assign(kinds_.size(), false);
    {
        std::vector<int> kindOfSubgroup(subgroups_.size(), -1);
        for (size_t k = 0; k < kinds_.size(); ++k)
            kindOfSubgroup[static_cast<size_t>(kinds_[k].subgroup)] = static_cast<int>(k);
        for (const Kind& k : kinds_)
        {
            if (k.role != Role::Resist)
                continue;
            for (const auto& [t, attr] : subgroups_[static_cast<size_t>(k.subgroup)].influence.optionFields)
            {
                int j = kindOfSubgroup[static_cast<size_t>(t)];
                if (j >= 0 && t != k.subgroup && kinds_[static_cast<size_t>(j)].role == Role::Resist)
                    resistAdaptive_[static_cast<size_t>(j)] = true;
            }
        }
    }
    auto envFlag = [](const char* name) {
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, name) == 0 && e;
        free(e);
        return on;
    };
    resistVerify_ = opt_.bruteForce || envFlag("OPT_RESIST_VERIFY");
    noAnalytic_ = envFlag("OPT_NO_ANALYTIC");
    auto multisets = [&](const std::vector<int>& list, int r) {
        std::vector<int> caps, groups;
        for (int k : list)
        {
            caps.push_back(kinds_[static_cast<size_t>(k)].cap);
            groups.push_back(kinds_[static_cast<size_t>(k)].groupID);
        }
        return rackMultisets(list, slots_[static_cast<size_t>(r)], caps, groups, groupCap);
    };
    for (int r = 0; r < RackCount; ++r)
    {
        resistMs_[static_cast<size_t>(r)] = multisets(resistKinds_[static_cast<size_t>(r)], r);
        fitMs_[static_cast<size_t>(r)] = multisets(fitKinds_[static_cast<size_t>(r)], r);
        coreMs_[static_cast<size_t>(r)] = multisets(coreKinds_[static_cast<size_t>(r)], r);
    }
    auto product = [&](const std::array<std::vector<Multiset>, RackCount>& ms) {
        std::vector<SetRef> out;
        SetRef cur;
        std::function<void(int)> rec = [&](int r) {
            if (r == RackCount)
            {
                out.push_back(cur);
                return;
            }
            for (size_t i = 0; i < ms[static_cast<size_t>(r)].size(); ++i)
            {
                cur.ms[static_cast<size_t>(r)] = static_cast<uint32_t>(i);
                cur.size[static_cast<size_t>(r)] = ms[static_cast<size_t>(r)][i].n;
                rec(r + 1);
            }
        };
        rec(0);
        return out;
    };
    for (int r = 0; r < RackCount; ++r)
    {
        // Enumeration is depth first: a multiset's extensions follow it, larger.
        const auto& list = resistMs_[static_cast<size_t>(r)];
        auto& skip = resistSkip_[static_cast<size_t>(r)];
        skip.assign(list.size(), list.size());
        std::vector<size_t> open;
        for (size_t j = 0; j < list.size(); ++j)
        {
            while (!open.empty() && list[open.back()].n >= list[j].n)
            {
                skip[open.back()] = j;
                open.pop_back();
            }
            open.push_back(j);
        }
    }
    resistSets_ = product(resistMs_);
    fitSets_ = product(fitMs_);
    std::stable_sort(fitSets_.begin(), fitSets_.end(), [](const SetRef& a, const SetRef& b) {
        return a.size[0] + a.size[1] + a.size[2] < b.size[0] + b.size[1] + b.size[2];
    });
    result_.stats.resistSets = static_cast<double>(resistSets_.size());

    // Resist plan: resist kinds plus the core kinds that change resists, per rack.
    // Fitting plan: fitting kinds plus the core kinds that change outputs or their costs.
    auto makePlan = [&](const std::array<std::vector<int>, RackCount>& own, const std::vector<int>& extra,
                        std::array<std::vector<int>, RackCount>& rackOptions, std::array<int, RackCount>& firstSlot,
                        std::vector<int>& optionKind) {
        SearchSpec spec;
        spec.base = base_;
        for (int r = 0; r < RackCount; ++r)
        {
            auto& opts = rackOptions[static_cast<size_t>(r)];
            opts = own[static_cast<size_t>(r)];
            for (int k : extra)
                if (kinds_[static_cast<size_t>(k)].rack == r)
                    opts.push_back(k);
            firstSlot[static_cast<size_t>(r)] = static_cast<int>(spec.slots.size());
            if (opts.empty())
                continue;
            std::vector<ModuleInput> inputs;
            for (int k : opts)
                inputs.push_back(cands_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].cand)].input);
            for (int s = 0; s < slots_[static_cast<size_t>(r)]; ++s)
                spec.slots.push_back(inputs);
        }
        auto plan = std::make_unique<SearchPlan>(engine_, spec);
        optionKind.assign(static_cast<size_t>(plan->optionCount()), -1);
        for (int r = 0; r < RackCount; ++r)
        {
            const auto& opts = rackOptions[static_cast<size_t>(r)];
            if (opts.empty())
                continue;
            for (int s = 0; s < slots_[static_cast<size_t>(r)]; ++s)
                for (size_t l = 0; l < opts.size(); ++l)
                    optionKind[static_cast<size_t>(
                        plan->optionIndex(firstSlot[static_cast<size_t>(r)] + s, static_cast<int>(l)))] = opts[l];
        }
        return plan;
    };
    resistPlan_ = makePlan(resistKinds_, eps_, resistRackOptions_, resistFirstSlot_, resistOptionKind_);
    fitPlan_ = makePlan(fitKinds_, inflFit_, fitRackOptions_, fitFirstSlot_, fitOptionKind_);
    if (eps_.size() > 24 || inflFit_.size() > 24)
        throw ModelLimit("too many kinds change resists or fitting");
    logLine(std::to_string(resistSets_.size()) + " resist sets, " + std::to_string(fitSets_.size()) +
            " fitting-module sets");
}

// Places kinds (counts) and the multisets of one set into a plan's slots.
void placeChoice(std::vector<int>& choice, const std::array<std::vector<int>, RackCount>& rackOptions,
                 const std::array<int, RackCount>& firstSlot, const uint8_t* counts, const std::vector<int>& countKinds,
                 const std::array<const Multiset*, RackCount>& sets,
                 const std::array<std::vector<int>, RackCount>& setKinds, const std::vector<int>& kindRack)
{
    std::array<int, RackCount> next = firstSlot;
    auto localOf = [&](int r, int kind) {
        const auto& opts = rackOptions[static_cast<size_t>(r)];
        return static_cast<int>(std::find(opts.begin(), opts.end(), kind) - opts.begin());
    };
    for (int k : countKinds)
    {
        int r = kindRack[static_cast<size_t>(k)];
        for (int c = 0; c < counts[k]; ++c)
            choice[static_cast<size_t>(next[static_cast<size_t>(r)]++)] = localOf(r, k);
    }
    for (int r = 0; r < RackCount; ++r)
        if (const Multiset* m = sets[static_cast<size_t>(r)])
            for (int i = 0; i < m->n; ++i)
                choice[static_cast<size_t>(next[static_cast<size_t>(r)]++)] =
                    localOf(r, setKinds[static_cast<size_t>(r)][m->k[static_cast<size_t>(i)]]);
}

// One resist-plan test: the eps kinds (counts) and the given multisets in their racks.
void Search::evaluateResist(const uint8_t* counts, const std::array<const Multiset*, RackCount>& sets, TestDetail& d)
{
    int slotCount = 0;
    for (int r = 0; r < RackCount; ++r)
        if (!resistRackOptions_[static_cast<size_t>(r)].empty())
            slotCount = resistFirstSlot_[static_cast<size_t>(r)] + slots_[static_cast<size_t>(r)];
    static thread_local std::vector<int> choice;
    choice.assign(static_cast<size_t>(slotCount), -1);
    placeChoice(choice, resistRackOptions_, resistFirstSlot_, counts, eps_, sets, resistKinds_, kindRack_);
    resistPlan_->evaluate(choice.data(), goal_, &d);
    calls_.fetch_add(1, std::memory_order_relaxed);
}

const Search::ResistRec& Search::resistRec(ResistTable& t, int set)
{
    const size_t c = static_cast<size_t>(set) / ResistTable::kChunk, i = static_cast<size_t>(set) % ResistTable::kChunk;
    if (ResistSlot* chunk = t.chunks[c].load(std::memory_order_acquire))
        if (chunk[i].ready.load(std::memory_order_acquire))
            return chunk[i].rec;
    // D, capacitor use and fitting cost of the set's resist modules.
    static thread_local TestDetail d;
    const SetRef& ref = resistSets_[static_cast<size_t>(set)];
    std::array<const Multiset*, RackCount> sets{};
    for (int r = 0; r < RackCount; ++r)
        sets[static_cast<size_t>(r)] = &resistMs_[static_cast<size_t>(r)][ref.ms[static_cast<size_t>(r)]];
    evaluateResist(t.counts.data(), sets, d);
    resistEvals_.fetch_add(1, std::memory_order_relaxed);
    ResistRec rec;
    rec.d = d.divider[static_cast<size_t>(layer_)];
    double x = 0, cpu = 0, power = 0, cal = 0;
    for (int m = 0; m < d.moduleCount; ++m)
    {
        int opt = d.moduleOption[static_cast<size_t>(m)];
        if (opt < 0 || kinds_[static_cast<size_t>(resistOptionKind_[static_cast<size_t>(opt)])].role != Role::Resist)
            continue;
        const ModuleStats& ms = d.modules[static_cast<size_t>(m)];
        if (ms.state >= State::Active && ms.capNeed != 0 && ms.rawCycle + ms.reactivation > 0 && ms.capUse > 0)
            x += ms.capUse;
        cpu += ms.cpu;
        power += ms.power;
        cal += ms.calibration;
    }
    rec.x = static_cast<float>(x);
    rec.cpu = static_cast<float>(cpu);
    rec.power = static_cast<float>(power);
    rec.calibration = static_cast<float>(cal);

    std::lock_guard lock(t.m);
    ResistSlot* chunk = t.chunks[c].load(std::memory_order_relaxed);
    if (!chunk)
    {
        chunk = new ResistSlot[ResistTable::kChunk];
        t.chunks[c].store(chunk, std::memory_order_release);
    }
    if (!chunk[i].ready.load(std::memory_order_relaxed))
    {
        chunk[i].rec = rec;
        chunk[i].ready.store(true, std::memory_order_release);
    }
    return chunk[i].rec;
}

namespace
{
// A penalized multiplier in the analytic resonance model, kept sorted the way
// pyfa applies them: by penalty group, bonuses before maluses, strongest first.
struct Pen
{
    double v;
    uint8_t group, dir;  // dir: 0 bonus (v > 1), 1 malus (v < 1)
    double strength() const { return std::abs(v - 1); }
};
bool penBefore(const Pen& a, const Pen& b)
{
    if (a.group != b.group)
        return a.group < b.group;
    if (a.dir != b.dir)
        return a.dir < b.dir;
    return a.strength() > b.strength();
}
double penFactor(int rank)
{
    static const std::array<double, 64> f = [] {
        std::array<double, 64> t{};
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = std::exp(-static_cast<double>(i * i) / 7.1289);
        return t;
    }();
    return rank < 64 ? f[static_cast<size_t>(rank)] : 0.0;
}
// The penalized product of a sorted list, with an optional extra multiplier merged in.
double penProduct(const Pen* e, int n, const Pen* extra)
{
    double p = 1;
    int rank = 0, group = -1, dir = -1;
    auto apply = [&](const Pen& x) {
        if (x.group != group || x.dir != dir)
        {
            group = x.group;
            dir = x.dir;
            rank = 0;
        }
        p *= 1 + (x.v - 1) * penFactor(rank++);
    };
    for (int i = 0; i < n; ++i)
    {
        if (extra && penBefore(*extra, e[i]))
        {
            apply(*extra);
            extra = nullptr;
        }
        apply(e[i]);
    }
    if (extra)
        apply(*extra);
    return p;
}
// Sorts (non-negative bound, set) pairs given in ascending set order, like
// std::sort on the pairs: a stable LSD radix sort on the float bits (which order
// like the values for non-negative floats).
void sortBounds(std::vector<std::pair<float, int>>& v, std::vector<std::pair<float, int>>& tmp)
{
    if (v.size() < 256)
    {
        std::sort(v.begin(), v.end());
        return;
    }
    tmp.resize(v.size());
    auto bits = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        return u;
    };
    for (int shift = 0; shift < 32; shift += 11)
    {
        std::array<size_t, 2049> count{};
        for (const auto& e : v)
            ++count[(bits(e.first) >> shift & 2047) + 1];
        for (size_t i = 1; i < count.size(); ++i)
            count[i] += count[i - 1];
        for (const auto& e : v)
            tmp[count[bits(e.first) >> shift & 2047]++] = e;
        v.swap(tmp);
    }
}

void insertPen(std::vector<Pen>& list, const Pen& x)
{
    auto at = std::upper_bound(list.begin(), list.end(), x, penBefore);
    list.insert(at, x);
}
}  // namespace

// Lower bounds of D for every resist set that fits next to the table's eps modules
// (bound[s] stays -1 for the others), from the engine's composition of the layer's
// resonances (TestDetail::resonanceParts): the eps modules alone give the base
// value, preIncrease, unpenalized multiplier and penalized multipliers; each resist
// kind alone adds its multipliers. A set's resonances then follow pyfa's formula,
// computed in double here and padded by 1e-5: every set used is evaluated exactly
// anyway, the bound only orders them. The reactive hardener adapts to the others:
// its resonances can be any split of its total resist (a convex set), and the
// resonances are concave in each multiplier, so its best case is a vertex: all of
// its resist on one damage type. Returns false if the composition is anything
// else (forced values, additive changes, kinds that change each other): the table
// is then evaluated exactly.
bool Search::resistBounds(ResistTable& t, const std::array<int, RackCount>& epsCount, std::vector<float>& bound)
{
    static thread_local TestDetail d;
    d.wantResonanceParts = true;
    const size_t L = static_cast<size_t>(layer_);
    evaluateResist(t.counts.data(), {}, d);
    const std::array<AttrParts, 4> p0 = d.resonanceParts[L];
    std::array<std::vector<Pen>, 4> basePens;
    std::array<double, 4> basePre{}, basePost{};
    for (size_t k = 0; k < 4; ++k)
    {
        const AttrParts& p = p0[k];
        if (p.forced || p.truncated || p.mult == 0)
            return false;
        basePre[k] = static_cast<double>((p.base + p.preInc) * p.mult);
        basePost[k] = p.post;
        for (int i = 0; i < p.penCount; ++i)
            if (float v = p.pens[static_cast<size_t>(i)]; v != 1)
                insertPen(basePens[k], {v, p.groups[static_cast<size_t>(i)], static_cast<uint8_t>(v > 1 ? 0 : 1)});
    }
    auto analytic = [&](size_t k, const std::vector<Pen>& pens, double pre) {
        return pre * penProduct(pens.data(), static_cast<int>(pens.size()), nullptr) + basePost[k];
    };
    for (size_t k = 0; k < 4; ++k)
        if (std::abs(analytic(k, basePens[k], basePre[k]) - p0[k].value) > 1e-6 * std::abs(p0[k].value) + 1e-9)
            return false;

    // Each resist kind alone next to the eps modules.
    struct KindModel
    {
        bool present = false, rah = false;
        std::array<double, 4> mult{1, 1, 1, 1};
        std::array<std::vector<Pen>, 4> pens;
    };
    std::vector<KindModel> model(kinds_.size());
    double rahTotal = 0;
    uint8_t rahGroup = 0;
    size_t maxPens = 0;
    for (int r = 0; r < RackCount; ++r)
    {
        const size_t ri = static_cast<size_t>(r);
        if (epsCount[ri] + 1 > slots_[ri])
            continue;  // no set with a module in this rack fits
        size_t worst = 0;
        for (size_t l = 0; l < resistKinds_[ri].size(); ++l)
        {
            int j = resistKinds_[ri][l];
            if (kinds_[static_cast<size_t>(j)].cap < 1)
                continue;
            Multiset one;
            one.n = 1;
            one.k[0] = static_cast<uint8_t>(l);
            std::array<const Multiset*, RackCount> sets{};
            sets[ri] = &one;
            evaluateResist(t.counts.data(), sets, d);
            KindModel& km = model[static_cast<size_t>(j)];
            km.present = true;
            const Group* g = gd_.groupOf(*cands_[static_cast<size_t>(kinds_[static_cast<size_t>(j)].cand)].type);
            if (resistAdaptive_[static_cast<size_t>(j)])
            {
                if (!g || g->name != "Armor Resistance Shift Hardener")
                    return false;
                km.rah = true;
            }
            for (size_t k = 0; k < 4; ++k)
            {
                const AttrParts& p = d.resonanceParts[L][k];
                if (p.forced || p.truncated || p.base != p0[k].base || p.preInc != p0[k].preInc || p.post != p0[k].post)
                    return false;
                km.mult[k] = static_cast<double>(p.mult) / p0[k].mult;
                // Its multipliers: this list minus the eps modules' (all of which must remain).
                std::vector<std::pair<float, uint8_t>> rest;
                for (int i = 0; i < p.penCount; ++i)
                    rest.push_back({p.pens[static_cast<size_t>(i)], p.groups[static_cast<size_t>(i)]});
                for (int i = 0; i < p0[k].penCount; ++i)
                {
                    auto it = std::find(rest.begin(), rest.end(),
                                        std::pair<float, uint8_t>{p0[k].pens[static_cast<size_t>(i)],
                                                                  p0[k].groups[static_cast<size_t>(i)]});
                    if (it == rest.end())
                        return false;
                    rest.erase(it);
                }
                std::vector<Pen> all = basePens[k];
                for (const auto& [v, grp] : rest)
                    if (v != 1)
                    {
                        Pen x{v, grp, static_cast<uint8_t>(v > 1 ? 0 : 1)};
                        insertPen(km.pens[k], x);
                        insertPen(all, x);
                    }
                if (std::abs(analytic(k, all, basePre[k] * km.mult[k]) - p.value) > 1e-6 * std::abs(p.value) + 1e-9)
                    return false;
                worst = std::max(worst, km.pens[k].size());
            }
            if (km.rah)
            {
                // Its total resist (conserved by the adaptation); one multiplier per type.
                double total = 0;
                for (size_t k = 0; k < 4; ++k)
                {
                    if (km.mult[k] != 1 || km.pens[k].size() > 1)
                        return false;
                    if (!km.pens[k].empty())
                    {
                        total += 1 - km.pens[k][0].v;
                        rahGroup = km.pens[k][0].group;
                    }
                }
                rahTotal = total;
                km.pens = {};
            }
        }
        maxPens += worst * static_cast<size_t>(slots_[ri]);
    }
    for (size_t k = 0; k < 4; ++k)
        if (basePens[k].size() + maxPens > 60)
            return false;

    // Every set, walking each rack's multisets in enumeration order (each one
    // extends a prefix of the previous one) with the multipliers kept sorted, and
    // skipping the multisets (and their extensions) that don't fit.
    constexpr int kCap = 64, kKindPens = 8;
    struct List
    {
        int n = 0;
        std::array<Pen, kCap> e;
    };
    std::array<List, 4> pens;
    for (size_t k = 0; k < 4; ++k)
    {
        pens[k].n = static_cast<int>(basePens[k].size());
        std::copy(basePens[k].begin(), basePens[k].end(), pens[k].e.begin());
    }
    for (const KindModel& km : model)
        for (const auto& p : km.pens)
            if (p.size() > kKindPens)
                return false;
    std::array<double, 4> pre = basePre;
    int rahCount = 0;
    struct Undo
    {
        std::array<double, 4> pre;
        std::array<std::array<uint8_t, kKindPens>, 4> at;
        std::array<uint8_t, 4> count;
    };
    std::array<Undo, RackCount * kMaxRackSlots + 1> undo;
    int depth = 0;
    auto push = [&](int j) {
        const KindModel& km = model[static_cast<size_t>(j)];
        Undo& u = undo[static_cast<size_t>(depth++)];
        u.pre = pre;
        for (size_t k = 0; k < 4; ++k)
        {
            pre[k] *= km.mult[k];
            List& l = pens[k];
            u.count[k] = static_cast<uint8_t>(km.pens[k].size());
            for (size_t p = 0; p < km.pens[k].size(); ++p)
            {
                const Pen& x = km.pens[k][p];
                int at = static_cast<int>(std::upper_bound(l.e.begin(), l.e.begin() + l.n, x, penBefore) - l.e.begin());
                std::copy_backward(l.e.begin() + at, l.e.begin() + l.n, l.e.begin() + l.n + 1);
                l.e[static_cast<size_t>(at)] = x;
                ++l.n;
                u.at[k][p] = static_cast<uint8_t>(at);
            }
        }
        rahCount += km.rah;
    };
    auto pop = [&](int j) {
        const Undo& u = undo[static_cast<size_t>(--depth)];
        for (size_t k = 0; k < 4; ++k)
        {
            List& l = pens[k];
            for (int p = u.count[k] - 1; p >= 0; --p)
            {
                int at = u.at[k][static_cast<size_t>(p)];
                std::copy(l.e.begin() + at + 1, l.e.begin() + l.n, l.e.begin() + at);
                --l.n;
            }
        }
        pre = u.pre;
        rahCount -= model[static_cast<size_t>(j)].rah;
    };
    const Pen rahVertex{1 - rahTotal, rahGroup, 1};
    auto leaf = [&](size_t set) {
        std::array<double, 4> w;
        double sum = 0;
        for (size_t k = 0; k < 4; ++k)
        {
            w[k] = pre[k] * penProduct(pens[k].e.data(), pens[k].n, nullptr) + basePost[k];
            sum += w[k];
        }
        if (rahCount == 1)
        {
            double best = std::numeric_limits<double>::infinity();
            for (size_t k = 0; k < 4; ++k)
                best = std::min(best, pre[k] * penProduct(pens[k].e.data(), pens[k].n, &rahVertex) + basePost[k] - w[k]);
            sum += best;
        }
        else if (rahCount > 1)
            sum = 0;
        bound[set] = static_cast<float>(std::max(0.0, 0.25 * sum * (1 - 1e-5)));
    };
    std::array<size_t, RackCount> radix{};
    for (int r = 0; r < RackCount; ++r)
        radix[static_cast<size_t>(r)] = resistMs_[static_cast<size_t>(r)].size();
    std::function<void(int, size_t)> walk = [&](int r, size_t index) {
        const size_t ri = static_cast<size_t>(r);
        const auto& list = resistMs_[ri];
        const auto& skip = resistSkip_[ri];
        const int room = slots_[ri] - epsCount[ri];
        Multiset cur;
        for (size_t i = 0; i < list.size();)
        {
            const Multiset& m = list[i];
            if (m.n > room)
            {
                i = skip[i];
                continue;
            }
            int common = 0;
            while (common < cur.n && common < m.n && cur.k[static_cast<size_t>(common)] == m.k[static_cast<size_t>(common)])
                ++common;
            while (cur.n > common)
                pop(resistKinds_[ri][cur.k[static_cast<size_t>(--cur.n)]]);
            while (cur.n < m.n)
            {
                cur.k[cur.n] = m.k[cur.n];
                push(resistKinds_[ri][cur.k[cur.n++]]);
            }
            size_t idx = index * radix[ri] + i;
            if (r + 1 < RackCount)
                walk(r + 1, idx);
            else
                leaf(idx);
            i = m.n == room ? skip[i] : i + 1;
        }
        while (cur.n > 0)
            pop(resistKinds_[ri][cur.k[static_cast<size_t>(--cur.n)]]);
    };
    walk(0, 0);
    return true;
}


void Search::buildResistTable(ResistTable& t)
{
    const size_t n = resistSets_.size();
    t.chunks = std::vector<std::atomic<ResistSlot*>>((n + ResistTable::kChunk - 1) / ResistTable::kChunk);
    std::array<int, RackCount> epsCount{};
    for (int k : eps_)
        epsCount[static_cast<size_t>(kinds_[static_cast<size_t>(k)].rack)] += t.counts[static_cast<size_t>(k)];
    std::vector<float> bound(n, -1.0f);
    resistTableCount_.fetch_add(1, std::memory_order_relaxed);
    auto t0 = Clock::now();
    if (noAnalytic_ || !resistBounds(t, epsCount, bound))
    {
        resistExactTables_.fetch_add(1, std::memory_order_relaxed);
        t.exact = true;
        for (size_t s = 0; s < n; ++s)
        {
            bool fits = true;
            for (int r = 0; r < RackCount; ++r)
                fits &= epsCount[static_cast<size_t>(r)] + resistSets_[s].size[static_cast<size_t>(r)] <=
                        slots_[static_cast<size_t>(r)];
            bound[s] = fits ? resistRec(t, static_cast<int>(s)).d : -1.0f;
        }
    }
    if (resistVerify_ && !t.exact)
        for (size_t s = 0; s < n; ++s)
            if (bound[s] >= 0 && resistRec(t, static_cast<int>(s)).d < bound[s])
                resistViolations_.fetch_add(1, std::memory_order_relaxed);
    auto t1 = Clock::now();
    std::vector<int> groupOf(9 * 9 * 9, -1);
    for (size_t s = 0; s < n; ++s)
    {
        if (bound[s] < 0)
            continue;
        const auto& size = resistSets_[s].size;
        int& g = groupOf[static_cast<size_t>(size[0] + 9 * size[1] + 81 * size[2])];
        if (g < 0)
        {
            g = static_cast<int>(t.groups.size());
            t.groups.emplace_back();
            t.groupSize.push_back({size[0], size[1], size[2]});
        }
        t.groups[static_cast<size_t>(g)].push_back({bound[s], static_cast<int>(s)});
    }
    std::vector<std::pair<float, int>> tmp;
    for (auto& g : t.groups)
        sortBounds(g, tmp);
    auto t2 = Clock::now();
    resistBoundNs_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    resistSortNs_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
}

Search::ResistTable& Search::resistTable(const uint8_t* counts)
{
    Key key = packCounts(eps_, counts);
    const auto& slot = resistTables_.get(key, [] { return std::make_shared<ResistTableSlot>(); });
    std::call_once(slot->once, [&] {
        auto t = std::make_unique<ResistTable>();
        t->counts.assign(kinds_.size(), 0);
        for (int k : eps_)
            t->counts[static_cast<size_t>(k)] = counts[k];
        buildResistTable(*t);
        slot->table = std::move(t);
    });
    return *slot->table;
}

Search::ResistOrder& Search::resistOrder(const uint8_t* counts, const std::array<int, RackCount>& free)
{
    Key key = packCounts(eps_, counts);
    key.b ^= static_cast<uint64_t>(freeCode(free)) << 40;
    const auto& o = resistOrders_.get(key, [&] {
        auto order = std::make_shared<ResistOrder>();
        ResistTable& t = resistTable(counts);
        order->table = &t;
        size_t total = 0;
        for (size_t g = 0; g < t.groups.size(); ++g)
        {
            bool ok = !t.groups[g].empty();
            for (int r = 0; r < RackCount; ++r)
                ok &= t.groupSize[g][static_cast<size_t>(r)] <= free[static_cast<size_t>(r)];
            if (!ok)
                continue;
            total += t.groups[g].size();
            order->pending.push_back({t.groups[g][0].first, t.groups[g][0].second, static_cast<int>(g), 0});
        }
        std::make_heap(order->pending.begin(), order->pending.end(), [](const auto& a, const auto& b) {
            return a.bound != b.bound ? a.bound > b.bound : a.set > b.set;
        });
        order->chunks.resize((total + ResistTable::kChunk - 1) / ResistTable::kChunk);
        return order;
    });
    return *o;
}

// One step of extending an order (caller holds o.m): emit the next set, or claim
// a pending set to evaluate (outside the lock: several threads can evaluate sets
// of one order at once), or wait for another thread's evaluation, or report the end.
Search::OrderStep Search::stepOrder(ResistOrder& o, int& set, float& bound)
{
    auto pendingAfter = [](const ResistOrder::Cursor& a, const ResistOrder::Cursor& b) {
        return a.bound != b.bound ? a.bound > b.bound : a.set > b.set;
    };
    auto readyAfter = [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a > b; };
    // Every unevaluated set has D >= its bound >= the smallest pending or in-flight bound.
    float gate = o.pending.empty() ? std::numeric_limits<float>::infinity() : o.pending.front().bound;
    for (float b : o.inFlight)
        gate = std::min(gate, b);
    if (!o.ready.empty() && o.ready.front().first < gate)
    {
        std::pop_heap(o.ready.begin(), o.ready.end(), readyAfter);
        int s = o.ready.back().second;
        o.ready.pop_back();
        size_t n = o.size.load(std::memory_order_relaxed);
        auto& chunk = o.chunks[n / ResistTable::kChunk];
        if (!chunk)
            chunk = std::make_unique<int[]>(ResistTable::kChunk);
        chunk[n % ResistTable::kChunk] = s;
        o.size.store(n + 1, std::memory_order_release);
        return OrderStep::Emitted;
    }
    if (!o.pending.empty())
    {
        std::pop_heap(o.pending.begin(), o.pending.end(), pendingAfter);
        ResistOrder::Cursor c = o.pending.back();
        o.pending.pop_back();
        set = c.set;
        bound = c.bound;
        const auto& group = o.table->groups[static_cast<size_t>(c.group)];
        if (++c.pos < group.size())
        {
            c.bound = group[c.pos].first;
            c.set = group[c.pos].second;
            o.pending.push_back(c);
            std::push_heap(o.pending.begin(), o.pending.end(), pendingAfter);
        }
        o.inFlight.push_back(bound);
        return OrderStep::Evaluate;
    }
    return o.inFlight.empty() ? OrderStep::Done : OrderStep::Wait;
}

bool Search::resistAt(ResistOrder& o, size_t i, int& set)
{
    if (i >= o.size.load(std::memory_order_acquire))
    {
        std::unique_lock lock(o.m);
        while (o.size.load(std::memory_order_relaxed) <= i)
        {
            int s = -1;
            float bound = 0;
            switch (stepOrder(o, s, bound))
            {
            case OrderStep::Emitted: break;
            case OrderStep::Done: return false;
            case OrderStep::Wait: o.cv.wait(lock); break;
            case OrderStep::Evaluate:
            {
                lock.unlock();
                float d = resistRec(*o.table, s).d;
                if (d < bound)
                    resistViolations_.fetch_add(1, std::memory_order_relaxed);
                lock.lock();
                o.inFlight.erase(std::find(o.inFlight.begin(), o.inFlight.end(), bound));
                o.ready.push_back({d, s});
                std::push_heap(o.ready.begin(), o.ready.end(),
                               [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a > b; });
                o.cv.notify_all();
                break;
            }
            }
        }
    }
    set = o.chunks[i / ResistTable::kChunk][i % ResistTable::kChunk];
    return true;
}


double Search::minD(const uint8_t* counts, const std::array<int, RackCount>& free)
{
    ResistOrder& o = resistOrder(counts, free);
    int s;
    if (!resistAt(o, 0, s))
        return std::numeric_limits<double>::infinity();
    return resistRec(*o.table, s).d;
}

const Search::FitRecord& Search::fitRecord(const uint8_t* counts, int fset)
{
    Key key = packCounts(inflFit_, counts);
    key.b ^= static_cast<uint64_t>(fset + 1) << 40;
    return fitRecs_.get(key, [&] {
        std::vector<int> kindRack(kinds_.size());
        for (size_t k = 0; k < kinds_.size(); ++k)
            kindRack[k] = kinds_[k].rack;
        int slotCount = 0;
        for (int r = 0; r < RackCount; ++r)
            if (!fitRackOptions_[static_cast<size_t>(r)].empty())
                slotCount = fitFirstSlot_[static_cast<size_t>(r)] + slots_[static_cast<size_t>(r)];
        std::vector<int> choice(static_cast<size_t>(slotCount), -1);
        std::array<const Multiset*, RackCount> sets{};
        const SetRef& ref = fitSets_[static_cast<size_t>(fset)];
        for (int r = 0; r < RackCount; ++r)
            sets[static_cast<size_t>(r)] = &fitMs_[static_cast<size_t>(r)][ref.ms[static_cast<size_t>(r)]];
        placeChoice(choice, fitRackOptions_, fitFirstSlot_, counts, inflFit_, sets, fitKinds_, kindRack);
        static thread_local TestDetail d;
        TestResult res = fitPlan_->evaluate(choice.data(), goal_, &d);
        calls_.fetch_add(1, std::memory_order_relaxed);
        FitRecord rec;
        rec.outputs = {res.cpuOutput, res.powerOutput, res.calibrationOutput};
        for (int i = 0; i < d.moduleCount; ++i)
        {
            int opt = d.moduleOption[static_cast<size_t>(i)];
            if (opt < 0 || kinds_[static_cast<size_t>(fitOptionKind_[static_cast<size_t>(opt)])].role != Role::Fitting)
                continue;
            rec.cost[0] += d.modules[static_cast<size_t>(i)].cpu;
            rec.cost[1] += d.modules[static_cast<size_t>(i)].power;
            rec.cost[2] += d.modules[static_cast<size_t>(i)].calibration;
        }
        return rec;
    });
}

// The most CPU / power / calibration left over for the core and resist modules
// with the best fitting-module set in the free slots, each resource on its own: a
// necessary condition for fitting.
std::array<double, 3> Search::fitSlack(const uint8_t* counts, const std::array<int, RackCount>& free)
{
    Key key = packCounts(inflFit_, counts);
    key.b ^= static_cast<uint64_t>(freeCode(free)) << 40;
    return fitSlack_.get(key, [&] {
        std::array<double, 3> slack;
        slack.fill(-std::numeric_limits<double>::infinity());
        for (size_t f = 0; f < fitSets_.size(); ++f)
        {
            const SetRef& ref = fitSets_[f];
            bool ok = true;
            for (int r = 0; r < RackCount; ++r)
                ok &= ref.size[static_cast<size_t>(r)] <= free[static_cast<size_t>(r)];
            if (!ok)
                continue;
            const FitRecord& rec = fitRecord(counts, static_cast<int>(f));
            for (size_t i = 0; i < 3; ++i)
                slack[i] = std::max(slack[i], static_cast<double>(rec.outputs[i]) - rec.cost[i]);
        }
        return slack;
    });
}

// The best damage factor among resist sets that leave the fit a chance to fit.
double Search::feasibleMinD(const uint8_t* counts, const std::array<int, RackCount>& free, const CoreEval& ce,
                            const std::array<double, 3>& slack)
{
    constexpr double tol = 0.01;  // CPU / power are rounded to 2 decimals
    if (noFitPrune_)
        return minD(counts, free);
    if (ce.cpu > slack[0] + tol || ce.power > slack[1] + tol || ce.calibration > slack[2] + tol)
        return std::numeric_limits<double>::infinity();
    ResistOrder& o = resistOrder(counts, free);
    int s;
    for (size_t i = 0; resistAt(o, i, s); ++i)
    {
        const ResistRec& rec = resistRec(*o.table, s);
        if (ce.cpu + rec.cpu <= slack[0] + tol && ce.power + rec.power <= slack[1] + tol &&
            ce.calibration + rec.calibration <= slack[2] + tol)
            return rec.d;
    }
    return std::numeric_limits<double>::infinity();
}

// The first fitting-module set (fewest modules first) in the free slots that makes
// the fit's CPU, power and calibration fit; -1 if none does.
int Search::findFitting(const uint8_t* counts, const std::array<int, RackCount>& free, double cpu, double power,
                        double calibration)
{
    for (size_t f = 0; f < fitSets_.size(); ++f)
    {
        const SetRef& ref = fitSets_[f];
        bool ok = true;
        for (int r = 0; r < RackCount; ++r)
            ok &= ref.size[static_cast<size_t>(r)] <= free[static_cast<size_t>(r)];
        if (!ok)
            continue;
        const FitRecord& rec = fitRecord(counts, static_cast<int>(f));
        float c = pymath::round(static_cast<float>(cpu + rec.cost[0]), 2);
        float p = pymath::round(static_cast<float>(power + rec.cost[1]), 2);
        float k = static_cast<float>(calibration + rec.cost[2]);
        if (c <= rec.outputs[0] && p <= rec.outputs[1] && k <= rec.outputs[2])
            return static_cast<int>(f);
    }
    return -1;
}
}  // namespace

namespace
{
// Every way the free racks (freeMask) can hold the given core kinds (per-kind
// caps, rack sizes): each projection lists kind indices with repeats.
const std::vector<std::vector<int>>& Search::projections(const std::vector<int>& kindsIn, unsigned freeMask)
{
    std::vector<int> ks;
    for (int k : kindsIn)
        if ((freeMask >> kinds_[static_cast<size_t>(k)].rack & 1u) && kinds_[static_cast<size_t>(k)].role == Role::Core)
            ks.push_back(k);
    std::sort(ks.begin(), ks.end());
    ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
    // Key: the kinds as a bit set (bits 0..124), the free racks in the top 3 bits.
    if (kinds_.size() > 125)
        throw ModelLimit("too many kinds for a projection key");
    Key key;
    key.b = static_cast<uint64_t>(freeMask & 7u) << 61;
    for (int k : ks)
        (k < 64 ? key.a : key.b) |= uint64_t{1} << (k % 64);
    return projCache_.get(key, [&] {
        std::vector<std::vector<int>> out{{}};
        for (int r = 0; r < RackCount; ++r)
        {
            if (!(freeMask >> r & 1u))
                continue;
            std::vector<int> list, caps, groups;
            for (int k : ks)
                if (kinds_[static_cast<size_t>(k)].rack == r)
                {
                    list.push_back(k);
                    caps.push_back(kinds_[static_cast<size_t>(k)].cap);
                    groups.push_back(-1 - k);
                }
            if (list.empty())
                continue;
            auto ms = rackMultisets(list, slots_[static_cast<size_t>(r)], caps, groups, {});
            std::vector<std::vector<int>> next;
            for (const auto& prefix : out)
                for (const Multiset& m : ms)
                {
                    auto v = prefix;
                    for (int i = 0; i < m.n; ++i)
                        v.push_back(list[m.k[static_cast<size_t>(i)]]);
                    next.push_back(std::move(v));
                }
            out = std::move(next);
        }
        return out;
    });
}

// Upper bound of every core that extends `fixed` (the core kinds of the racks
// outside freeMask) with core modules in the free racks.
//
// The free racks' slots are split between the value side (repairers, capacitor
// modules, HP modules) and the resist side (resist modules and core kinds that
// change resists); every split is tried. On the value side, capacitor
// projections are grouped by the slots they take, and the repairer knapsack only
// gets the slots left.
double Search::partialBound(const uint8_t* fixed, unsigned freeMask)
{
    std::vector<uint8_t> base(fixed, fixed + kinds_.size());
    std::vector<uint8_t> tmp;
    auto withProjection = [&](const std::vector<int>& proj) {
        tmp = base;
        for (int k : proj)
            ++tmp[static_cast<size_t>(k)];
        return tmp.data();
    };
    std::array<int, RackCount> fixedFree = slots_;
    for (size_t k = 0; k < kinds_.size(); ++k)
        fixedFree[static_cast<size_t>(kinds_[k].rack)] -= base[k];
    std::vector<int> freeRacks;
    for (int r = 0; r < RackCount; ++r)
        if (freeMask >> r & 1u)
            freeRacks.push_back(r);
    // Fitting room left by the fixed modules (at their cheapest) under the most output reachable.
    std::array<double, 3> room = outputMax_;
    for (size_t p = 0; p < basePinned_.size(); ++p)
        if (pinnedLive_[p])
        {
            room[0] -= basePinned_[p].cpu * costScale_;
            room[1] -= basePinned_[p].power * costScale_;
            room[2] -= basePinned_[p].calibration;
        }
    for (size_t k = 0; k < kinds_.size(); ++k)
        if (base[k])
        {
            const Candidate& c = cands_[static_cast<size_t>(kinds_[k].cand)];
            room[0] -= base[k] * c.cpu * costScale_;
            room[1] -= base[k] * c.power * costScale_;
            room[2] -= base[k] * c.calibration;
        }
    auto projectionFits = [&](const std::vector<int>& proj) {
        std::array<double, 3> cost{};
        for (int k : proj)
        {
            const Candidate& c = cands_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].cand)];
            cost[0] += c.cpu * costScale_;
            cost[1] += c.power * costScale_;
            cost[2] += c.calibration;
        }
        return cost[0] <= room[0] + 0.01 && cost[1] <= room[1] + 0.01 && cost[2] <= room[2] + 0.01;
    };
    // Size vectors over the free racks, encoded 4 bits per rack.
    auto sizeCode = [&](const std::vector<int>& proj) {
        std::array<int, RackCount> n{};
        for (int k : proj)
            ++n[static_cast<size_t>(kinds_[static_cast<size_t>(k)].rack)];
        return n;
    };
    auto fitsIn = [&](const std::array<int, RackCount>& need, const std::array<int, RackCount>& have) {
        for (int r : freeRacks)
            if (need[static_cast<size_t>(r)] > have[static_cast<size_t>(r)])
                return false;
        return true;
    };

    // Resist side: per eps projection, its size and what it leaves for the best D.
    struct EpsOption
    {
        std::array<int, RackCount> size;
        const std::vector<int>* proj;
    };
    std::vector<EpsOption> epsOptions;
    for (const auto& proj : projections(eps_, freeMask))
        epsOptions.push_back({sizeCode(proj), &proj});

    // Value side, passive: HP per projection size.  Active: budget per size, and
    // the repairer items.
    std::vector<std::pair<std::array<int, RackCount>, double>> valueBySize;  // size -> best value input
    auto keepMax = [&](const std::array<int, RackCount>& size, double v) {
        for (auto& [s, best] : valueBySize)
            if (s == size)
            {
                best = std::max(best, v);
                return;
            }
        valueBySize.push_back({size, v});
    };
    struct Item
    {
        double gain, capUse;
        int count;  // fixed-rack items: copies present; free-rack items: most copies
        int rack;   // -1: fixed
        double cpu = 0, power = 0;  // free-rack items: cheapest fitting cost
    };
    std::vector<Item> items;
    double fixedRep = 0;
    if (!active_)
    {
        if (inflHp_.empty())
            keepMax({}, baseHp_);
        else
            for (const auto& proj : projections(inflHp_, freeMask))
                if (projectionFits(proj))
                    keepMax(sizeCode(proj), hpRecord(withProjection(proj)));
    }
    else
    {
        auto addItem = [&](const std::vector<int>& infl, const std::function<ModuleStats(const uint8_t*)>& record,
                           int count, int self, int rack) {
            RepItem best{-std::numeric_limits<double>::infinity(), 0, std::numeric_limits<double>::infinity()};
            for (const auto& proj : projections(infl, freeMask))
            {
                const uint8_t* counts = withProjection(proj);
                if (self >= 0 && tmp[static_cast<size_t>(self)] == 0)
                    tmp[static_cast<size_t>(self)] = 1;
                RepItem it = decompose(record(counts), layer_);
                best.fixed = std::max(best.fixed, it.fixed);
                best.gain = std::max(best.gain, it.gain);
                best.capUse = std::min(best.capUse, it.capUse);
            }
            if (!std::isfinite(best.fixed))
                return;
            double capUse = std::isfinite(best.capUse) ? best.capUse : 0.0;
            if (rack < 0)
            {
                fixedRep += best.fixed * count;
                items.push_back({best.gain, capUse, count, -1});
            }
            else  // the fixed part counts per copy too: fold it into the gain at no capacitor cost
            {
                const Candidate& c = cands_[static_cast<size_t>(kinds_[static_cast<size_t>(self)].cand)];
                double cpu = c.cpu * costScale_, power = c.power * costScale_;
                if (best.fixed > 0)
                    items.push_back({best.fixed, 0.0, count, rack, cpu, power});
                items.push_back({best.gain, capUse, count, rack, best.fixed > 0 ? 0.0 : cpu, best.fixed > 0 ? 0.0 : power});
            }
        };
        for (size_t k = 0; k < kinds_.size(); ++k)
        {
            const Kind& kind = kinds_[k];
            if (kind.role != Role::Core || cands_[static_cast<size_t>(kind.cand)].rep <= kTiny)
                continue;
            bool free = freeMask >> kind.rack & 1u;
            if (!free && !base[k])
                continue;
            int kk = static_cast<int>(k);
            addItem(inflKind_[k], [&](const uint8_t* counts) { return kindRecord(kk, counts); },
                    free ? kind.cap : base[k], free ? kk : -1, free ? kind.rack : -1);
        }
        for (size_t p = 0; p < basePinned_.size(); ++p)
            if (pinnedLive_[p] && basePinned_[p].repEffects)
                addItem(inflPinned_[p],
                        [&](const uint8_t* counts) { return pinnedRecord(static_cast<int>(p), counts); }, 1, -1, -1);
        for (const auto& proj : projections(capAll_, freeMask))
            if (projectionFits(proj))
                keepMax(sizeCode(proj), budgetRecord(withProjection(proj)));
    }

    // Repair with this budget and these free slots per rack (two relaxations, the
    // smaller one is still an upper bound).
    std::vector<KnapItem> knap;
    std::vector<double> gains;
    auto repairBound = [&](double budget, const std::array<int, RackCount>& slotsLeft) {
        knap.clear();
        double u2 = fixedRep;
        for (const Item& it : items)
        {
            int count = it.rack < 0 ? it.count : std::min(it.count, slotsLeft[static_cast<size_t>(it.rack)]);
            if (count > 0)
                knap.push_back({it.gain, it.capUse, count});
            if (it.rack < 0)
                u2 += std::max(0.0, it.gain) * it.count;
        }
        double u1 = fixedRep + fractionalKnapsack(budget, knap);
        // Same with CPU and with power as the budget (free-rack items only; the fixed
        // ones are fitted already).
        double fixedGain = fixedRep;
        for (const Item& it : items)
            if (it.rack < 0)
                fixedGain += std::max(0.0, it.gain) * it.count;
        for (int res = 0; res < 2; ++res)
        {
            knap.clear();
            for (const Item& it : items)
            {
                if (it.rack < 0)
                    continue;
                int count = std::min(it.count, slotsLeft[static_cast<size_t>(it.rack)]);
                if (count > 0)
                    knap.push_back({it.gain, res == 0 ? it.cpu : it.power, count});
            }
            u1 = std::min(u1, fixedGain + fractionalKnapsack(room[static_cast<size_t>(res)], knap));
        }
        for (int r : freeRacks)
        {
            gains.clear();
            for (const Item& it : items)
                if (it.rack == r)
                    for (int c = 0; c < std::min(it.count, slotsLeft[static_cast<size_t>(r)]); ++c)
                        gains.push_back(it.gain);
            std::sort(gains.begin(), gains.end(), std::greater<>());
            for (int c = 0; c < std::min<int>(static_cast<int>(gains.size()), slotsLeft[static_cast<size_t>(r)]); ++c)
                u2 += std::max(0.0, gains[static_cast<size_t>(c)]);
        }
        return std::min(u1, u2);
    };

    // Every split of the free racks' slots between the value and the resist side.
    double best = 0;
    std::array<int, RackCount> valueSlots{};
    std::function<void(size_t)> split = [&](size_t i) {
        if (i < freeRacks.size())
        {
            int r = freeRacks[i];
            for (int n = 0; n <= fixedFree[static_cast<size_t>(r)]; ++n)
            {
                valueSlots[static_cast<size_t>(r)] = n;
                split(i + 1);
            }
            return;
        }
        // Resist side: the free racks' remaining slots (and the fixed racks' free ones).
        std::array<int, RackCount> resistSlots = fixedFree;
        for (int r : freeRacks)
            resistSlots[static_cast<size_t>(r)] = fixedFree[static_cast<size_t>(r)] - valueSlots[static_cast<size_t>(r)];
        double dMin = std::numeric_limits<double>::infinity();
        for (const EpsOption& e : epsOptions)
        {
            if (!fitsIn(e.size, resistSlots))
                continue;
            std::array<int, RackCount> free = resistSlots;
            for (int r : freeRacks)
                free[static_cast<size_t>(r)] -= e.size[static_cast<size_t>(r)];
            dMin = std::min(dMin, minD(withProjection(*e.proj), free));
        }
        if (!std::isfinite(dMin))
            return;
        double v = 0;
        for (const auto& [size, input] : valueBySize)
        {
            if (!fitsIn(size, valueSlots))
                continue;
            if (!active_)
                v = std::max(v, input);
            else
            {
                std::array<int, RackCount> left = valueSlots;
                for (int r : freeRacks)
                    left[static_cast<size_t>(r)] -= size[static_cast<size_t>(r)];
                v = std::max(v, repairBound(input, left));
            }
        }
        best = std::max(best, v / dMin);
    };
    split(0);
    return best * (1 + opt_.boundPad);
}

// Necessary condition for any completion to fit: the fixed core modules (at their
// cheapest) within the most output the ship can reach.
// Necessary condition for any completion to fit, exact when the free racks can't
// change fitting (no kind there changes outputs or fixed modules' costs): the fixed
// modules' measured costs within the most room any fitting-module set in the free
// slots leaves. Modules added later only cost more and leave fewer free slots, so
// a core that fails here fails feasibleMinD too.
bool Search::fixedFits(const uint8_t* fixed, unsigned freeMask, CoreEval& scratch)
{
    if (noFitPrune_)
        return true;
    auto inFree = [&](int k) { return (freeMask >> kinds_[static_cast<size_t>(k)].rack & 1u) != 0; };
    if (std::any_of(inflFit_.begin(), inflFit_.end(), inFree))
        return true;
    for (size_t k = 0; k < kinds_.size(); ++k)
        if (fixed[k] && std::any_of(inflKind_[k].begin(), inflKind_[k].end(), inFree))
            return true;
    for (size_t p = 0; p < inflPinned_.size(); ++p)
        if (pinnedLive_[p] && std::any_of(inflPinned_[p].begin(), inflPinned_[p].end(), inFree))
            return true;
    assemble(fixed, scratch);
    std::array<int, RackCount> free = slots_;
    for (size_t k = 0; k < kinds_.size(); ++k)
        free[static_cast<size_t>(kinds_[k].rack)] -= fixed[k];
    const std::array<double, 3> slack = fitSlack(fixed, free);
    constexpr double tol = 0.01;  // as feasibleMinD
    return scratch.cpu <= slack[0] + tol && scratch.power <= slack[1] + tol && scratch.calibration <= slack[2] + tol;
}

bool Search::partialFits(const uint8_t* fixed, unsigned freeMask)
{
    (void)freeMask;
    if (noFitPrune_)
        return true;
    std::array<double, 3> cost{};
    for (size_t p = 0; p < basePinned_.size(); ++p)
        if (pinnedLive_[p])
        {
            cost[0] += basePinned_[p].cpu * costScale_;
            cost[1] += basePinned_[p].power * costScale_;
            cost[2] += basePinned_[p].calibration;
        }
    for (size_t k = 0; k < kinds_.size(); ++k)
        if (fixed[k])
        {
            const Candidate& c = cands_[static_cast<size_t>(kinds_[k].cand)];
            cost[0] += fixed[k] * c.cpu * costScale_;
            cost[1] += fixed[k] * c.power * costScale_;
            cost[2] += fixed[k] * c.calibration;
        }
    for (size_t i = 0; i < 3; ++i)
        if (cost[i] > outputMax_[i] + 0.01)
            return false;
    return true;
}

void Search::countsOf(const std::array<uint32_t, RackCount>& ms, int level, std::vector<uint8_t>& counts) const
{
    std::fill(counts.begin(), counts.end(), uint8_t{0});
    for (int l = 0; l < level; ++l)
    {
        int r = rackOrder_[static_cast<size_t>(l)];
        const Multiset& m = coreMs_[static_cast<size_t>(r)][ms[static_cast<size_t>(r)]];
        for (int i = 0; i < m.n; ++i)
            ++counts[static_cast<size_t>(coreKinds_[static_cast<size_t>(r)][m.k[static_cast<size_t>(i)]])];
    }
}

void Search::stage1()
{
    // Racks with a choice of core modules, fixed one per level: the rack holding
    // most of the goal's own modules (repairers / HP) first, then rigs (they modify
    // everything else), then the rest by number of core multisets, fewest first.
    // On the Incursus test this order bounds 50x fewer cores than rigs first.
    rackOrder_.clear();
    std::array<int, RackCount> goalKinds{};
    for (const Kind& k : kinds_)
    {
        const Candidate& c = cands_[static_cast<size_t>(k.cand)];
        if (k.role == Role::Core && k.rack != RackRig && (active_ ? c.rep > kTiny : c.hpGain > kTiny))
            ++goalKinds[static_cast<size_t>(k.rack)];
    }
    for (int r = 0; r < RackCount; ++r)
        if (coreMs_[static_cast<size_t>(r)].size() > 1)
            rackOrder_.push_back(r);
    auto rank = [&](int r) { return r == RackRig ? 1 : goalKinds[static_cast<size_t>(r)] > 0 ? 0 : 2; };
    std::stable_sort(rackOrder_.begin(), rackOrder_.end(), [&](int a, int b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        if (rank(a) == 0 && goalKinds[static_cast<size_t>(a)] != goalKinds[static_cast<size_t>(b)])
            return goalKinds[static_cast<size_t>(a)] > goalKinds[static_cast<size_t>(b)];
        return coreMs_[static_cast<size_t>(a)].size() < coreMs_[static_cast<size_t>(b)].size();
    });
    char* env = nullptr;
    size_t envLen = 0;
    if (_dupenv_s(&env, &envLen, "OPT_RACK_ORDER") == 0 && env)
    {
        std::string o = env;
        free(env);
        std::stable_sort(rackOrder_.begin(), rackOrder_.end(),
                         [&](int a, int b) { return o.find(rackName(a)) < o.find(rackName(b)); });
    }
    const int levels = static_cast<int>(rackOrder_.size());
    {
        // Output bound: every rack filled with its strongest output modules, bonuses
        // compounded (stacking penalties only lower them). Cost bound: the strongest
        // cost-reducing rig in every rig slot.
        std::vector<uint8_t> none(kinds_.size(), 0);
        const FitRecord& baseRec = fitRecord(none.data(), 0);
        double maxCostGain = 0;
        for (size_t i = 0; i < 3; ++i)
        {
            double out = baseRec.outputs[i];
            for (int r = 0; r < RackCount; ++r)
            {
                std::vector<double> deltas;
                for (const Subgroup& sg : subgroups_)
                    for (int m : sg.members)
                        if (cands_[static_cast<size_t>(m)].rack == r)
                            deltas.push_back(cands_[static_cast<size_t>(m)].outputDelta[i]);
                std::sort(deltas.begin(), deltas.end(), std::greater<>());
                for (int n = 0; n < slots_[static_cast<size_t>(r)] && n < static_cast<int>(deltas.size()); ++n)
                    if (deltas[static_cast<size_t>(n)] > 0 && baseRec.outputs[i] > 0)
                        out *= 1 + deltas[static_cast<size_t>(n)] / baseRec.outputs[i];
            }
            outputMax_[i] = out;
        }
        for (const Subgroup& sg : subgroups_)
            for (int m : sg.members)
                if (cands_[static_cast<size_t>(m)].rack == RackRig)
                    maxCostGain = std::max(maxCostGain, cands_[static_cast<size_t>(m)].costGain);
        // Nothing raises calibration output or lowers a rig's calibration cost: the
        // calibration bounds use the base output and the full cost (costScale_ is
        // applied to CPU and power only).
        outputMax_[2] = baseRec.outputs[2];
        costScale_ = std::pow(std::max(0.0, 1 - maxCostGain), slots_[RackRig]);
        for (const Subgroup& sg : subgroups_)
            for (int m : sg.members)
                if (cands_[static_cast<size_t>(m)].rack != RackRig && cands_[static_cast<size_t>(m)].costGain > 0)
                    costScale_ = 0;  // non-rig cost reducers: no cheap cost bound
        // Pinned module costs can also be lowered by core kinds other than rigs:
        // then skip this check.
        for (const auto& infl : inflPinned_)
            if (!infl.empty())
                costScale_ = 0;
    }
    double coreSets = 1;
    std::string order;
    for (int r : rackOrder_)
    {
        coreSets *= static_cast<double>(coreMs_[static_cast<size_t>(r)].size());
        order += std::string(order.empty() ? "" : ", ") + rackName(r) + " (" +
                 std::to_string(coreMs_[static_cast<size_t>(r)].size()) + ")";
    }
    result_.stats.coreSets = coreSets;
    logLine("core sets " + std::to_string(coreSets) + ", racks fixed in order: " + (order.empty() ? "-" : order));

    auto freeOf = [&](const std::vector<uint8_t>& counts) {
        std::array<int, RackCount> free = slots_;
        for (size_t k = 0; k < kinds_.size(); ++k)
            free[static_cast<size_t>(kinds_[k].rack)] -= counts[k];
        return free;
    };

    // Groups whose limit spans more than one (rack, role) list: checked per set.
    std::vector<std::pair<int, int>> sharedLimits;  // groupID, limit
    {
        std::unordered_map<int, std::set<std::pair<int, int>>> lists;
        for (const Kind& k : kinds_)
            lists[k.groupID].insert({k.rack, static_cast<int>(k.role)});
        for (const auto& [g, l] : lists)
        {
            if (l.size() < 2)
                continue;
            for (const Kind& k : kinds_)
                if (k.groupID == g)
                {
                    const Candidate& c = cands_[static_cast<size_t>(k.cand)];
                    if (c.maxGroup)
                        sharedLimits.push_back({g, c.maxGroup - pinnedGroupCount_[g]});
                    break;
                }
        }
    }

    const size_t K = static_cast<size_t>(std::max(1, opt_.topK));
    top_.clear();
    auto cutoff = [&] { return top_.size() < K ? 0.0 : top_.front().score; };

    struct Node
    {
        double bound;
        std::array<uint32_t, RackCount> ms;
        int level;  // racks fixed (rackOrder_ prefix)
        bool operator<(const Node& o) const { return bound < o.bound; }
    };
    std::priority_queue<Node> queue;
    queue.push({std::numeric_limits<double>::infinity(), {}, 0});

    std::vector<std::vector<float>> boundLog(static_cast<size_t>(levels) + 1);
    const size_t workers = static_cast<size_t>(pool_->size());
    std::vector<std::vector<uint8_t>> childCounts(workers);
    std::vector<CoreEval> childEval(workers);
    std::vector<size_t> bounded(workers, 0);
    std::vector<double> childBound;
    // A whole core's sets that beat `cut`, in resist order. Its own finds raise its
    // cutoff too (K of them beat anything the global top-K would drop), so the list
    // holds every set the sequential scan would keep: merging lists in queue order
    // with the live cutoff (keep) gives the same top-K.
    std::vector<size_t> scored(workers, 0);
    auto scan = [&](const Node& node, double cut, std::vector<Found>& out, int worker) {
        std::vector<uint8_t>& counts = childCounts[static_cast<size_t>(worker)];
        CoreEval& ce = childEval[static_cast<size_t>(worker)];
        counts.resize(kinds_.size());
        countsOf(node.ms, levels, counts);
        assemble(counts.data(), ce);
        std::array<int, RackCount> free = freeOf(counts);
        ResistOrder& order = resistOrder(counts.data(), free);
        const std::array<double, 3> slack = fitSlack(counts.data(), free);
        std::vector<double> own;  // min-heap of this scan's scores, at most K
        auto localCut = [&] { return std::max(cut, own.size() < K ? 0.0 : own.front()); };
        int s;
        for (size_t pos = 0; resistAt(order, pos, s); ++pos)
        {
            const ResistRec& rr = resistRec(*order.table, s);
            double d = rr.d;
            if (ce.v0 / d * (1 + opt_.boundPad) <= localCut())
                break;
            if (ce.cpu + rr.cpu > slack[0] + 0.01 || ce.power + rr.power > slack[1] + 0.01 ||
                ce.calibration + rr.calibration > slack[2] + 0.01)
                continue;
            double score = coreValue(ce, rr.x) / d;
            ++scored[static_cast<size_t>(worker)];
            if (score <= localCut())
                continue;
            const SetRef& ref = resistSets_[static_cast<size_t>(s)];
            std::array<int, RackCount> left = free;
            for (int r = 0; r < RackCount; ++r)
                left[static_cast<size_t>(r)] -= ref.size[static_cast<size_t>(r)];
            int fset = findFitting(counts.data(), left, ce.cpu + rr.cpu, ce.power + rr.power,
                                   ce.calibration + rr.calibration);
            if (fset < 0)
                continue;
            if (!sharedLimits.empty())
            {
                bool ok = true;
                for (const auto& [g, limit] : sharedLimits)
                {
                    int n = 0;
                    for (size_t k = 0; k < kinds_.size(); ++k)
                        if (kinds_[k].groupID == g)
                            n += counts[k];
                    for (int r = 0; r < RackCount; ++r)
                    {
                        const Multiset& m = resistMs_[static_cast<size_t>(r)][ref.ms[static_cast<size_t>(r)]];
                        for (int i = 0; i < m.n; ++i)
                            n += kinds_[static_cast<size_t>(resistKinds_[static_cast<size_t>(r)][m.k[static_cast<size_t>(i)]])].groupID == g;
                        const Multiset& f = fitMs_[static_cast<size_t>(r)][fitSets_[static_cast<size_t>(fset)].ms[static_cast<size_t>(r)]];
                        for (int i = 0; i < f.n; ++i)
                            n += kinds_[static_cast<size_t>(fitKinds_[static_cast<size_t>(r)][f.k[static_cast<size_t>(i)]])].groupID == g;
                    }
                    ok &= n <= limit;
                }
                if (!ok)
                    continue;
            }
            out.push_back({score, node.ms, s, fset});
            own.push_back(score);
            std::push_heap(own.begin(), own.end(), std::greater<>());
            if (own.size() > K)
            {
                std::pop_heap(own.begin(), own.end(), std::greater<>());
                own.pop_back();
            }
        }
    };
    auto keep = [&](const Found& f) {
        if (f.score <= cutoff())
            return;
        if (top_.size() < K)
        {
            top_.push_back(f);
            std::push_heap(top_.begin(), top_.end());
        }
        else
        {
            std::pop_heap(top_.begin(), top_.end());
            top_.back() = f;
            std::push_heap(top_.begin(), top_.end());
        }
    };


    // Partial sets are expanded in batches: the best unexpanded ones up to the next
    // whole core (whose scan may raise the cutoff), at most kBatchNodes nodes or
    // kBatchChildren children. The children's bounds are computed in parallel (pure
    // functions of the child; the caches only memoize) and queued in order. The
    // batches don't depend on the thread count, so neither does the result.
    constexpr size_t kBatchNodes = 64, kBatchChildren = 4096;
    struct Batch
    {
        Node node;
        std::vector<uint8_t> counts;
        unsigned freeMask;
        size_t first;  // index of its first child
    };
    // Whole cores at the top of the queue are scanned the same way, up to
    // kBatchCores at a time, against the cutoff before the batch.
    constexpr size_t kBatchCores = 32;
    std::vector<Batch> batch;
    std::vector<Node> cores;
    std::vector<std::vector<Found>> finds;
    while (!queue.empty())
    {
        if (queue.top().bound <= cutoff())
            break;
        if (queue.top().level == levels)
        {
            cores.clear();
            while (!queue.empty() && cores.size() < kBatchCores && queue.top().level == levels &&
                   queue.top().bound > cutoff())
            {
                cores.push_back(queue.top());
                queue.pop();
            }
            const double cut = cutoff();
            finds.assign(cores.size(), {});
            pool_->run(cores.size(), 1, [&](size_t i, int worker) { scan(cores[i], cut, finds[i], worker); });
            for (size_t i = 0; i < cores.size(); ++i)
            {
                if (cores[i].bound <= cutoff())
                    continue;  // the sequential search would not have scanned it
                ++result_.stats.coresScanned;
                for (const Found& f : finds[i])
                    keep(f);
            }
            continue;
        }
        batch.clear();
        size_t total = 0;
        while (!queue.empty() && batch.size() < kBatchNodes && total < kBatchChildren &&
               queue.top().level < levels && queue.top().bound > cutoff())
        {
            Batch b;
            b.node = queue.top();
            queue.pop();
            b.freeMask = 0;
            for (int l = b.node.level + 1; l < levels; ++l)
                b.freeMask |= 1u << rackOrder_[static_cast<size_t>(l)];
            b.counts.resize(kinds_.size());
            countsOf(b.node.ms, b.node.level, b.counts);
            b.first = total;
            total += coreMs_[static_cast<size_t>(rackOrder_[static_cast<size_t>(b.node.level)])].size();
            batch.push_back(std::move(b));
            ++result_.stats.combosOpened;
        }
        bool partial = false;
        for (const Batch& b : batch)
            partial |= b.node.level + 1 < levels;
        childBound.assign(total, std::numeric_limits<double>::quiet_NaN());
        pool_->run(total, partial ? 1 : 16, [&](size_t i, int worker) {
            size_t bi = static_cast<size_t>(
                std::upper_bound(batch.begin(), batch.end(), i, [](size_t x, const Batch& b) { return x < b.first; }) -
                batch.begin() - 1);
            const Batch& b = batch[bi];
            const int r = rackOrder_[static_cast<size_t>(b.node.level)];
            const int level = b.node.level + 1;
            const Multiset& m = coreMs_[static_cast<size_t>(r)][i - b.first];
            const auto& list = coreKinds_[static_cast<size_t>(r)];
            std::vector<uint8_t>& ch = childCounts[static_cast<size_t>(worker)];
            ch = b.counts;
            for (int j = 0; j < m.n; ++j)
                ++ch[static_cast<size_t>(list[m.k[static_cast<size_t>(j)]])];
            if (level == levels)
            {
                // A whole core: its exact value over the best resist set it leaves room for.
                if (!partialFits(ch.data(), 0))
                    return;
                CoreEval& e = childEval[static_cast<size_t>(worker)];
                assemble(ch.data(), e);
                std::array<int, RackCount> childFree = freeOf(ch);
                double d = feasibleMinD(ch.data(), childFree, e, fitSlack(ch.data(), childFree));
                ++bounded[static_cast<size_t>(worker)];
                if (!std::isfinite(d) || e.v0 <= 0)
                    return;
                childBound[i] = e.v0 / d * (1 + opt_.boundPad);
            }
            else if (partialFits(ch.data(), b.freeMask) &&
                     fixedFits(ch.data(), b.freeMask, childEval[static_cast<size_t>(worker)]))
                childBound[i] = partialBound(ch.data(), b.freeMask);
        });
        for (const Batch& b : batch)
        {
            const int r = rackOrder_[static_cast<size_t>(b.node.level)];
            const size_t n = coreMs_[static_cast<size_t>(r)].size();
            for (size_t i = 0; i < n; ++i)
            {
                double bound = childBound[b.first + i];
                if (std::isnan(bound))
                    continue;
                Node c = b.node;
                c.ms[static_cast<size_t>(r)] = static_cast<uint32_t>(i);
                c.level = b.node.level + 1;
                c.bound = bound;
                if (c.bound > cutoff())
                    queue.push(c);
                boundLog[static_cast<size_t>(c.level)].push_back(static_cast<float>(c.bound));
            }
        }
    }
    for (size_t b : bounded)
        result_.stats.coresBounded += b;
    for (size_t n : scored)
        result_.stats.setsScored += n;

    if (log_ && !top_.empty())
    {
        double cut = cutoff();
        for (size_t l = 0; l < boundLog.size(); ++l)
        {
            auto& v = boundLog[l];
            if (v.empty())
                continue;
            std::sort(v.begin(), v.end());
            size_t above = static_cast<size_t>(v.end() - std::upper_bound(v.begin(), v.end(), static_cast<float>(cut)));
            *log_ << "  level " << l << ": " << v.size() << " bounds, " << above << " above the final cutoff; median "
                  << v[v.size() / 2] / cut << "x, max " << v.back() / cut << "x\n";
        }
    }
    std::sort(top_.begin(), top_.end(), [](const Found& a, const Found& b) { return a.score > b.score; });
    logLine(std::to_string(result_.stats.combosOpened) + " partial sets expanded, " +
            std::to_string(result_.stats.coresBounded) + " cores bounded, " +
            std::to_string(result_.stats.coresScanned) + " scanned, " + std::to_string(result_.stats.setsScored) +
            " sets scored; " + std::to_string(top_.size()) + " sets kept" +
            (top_.empty() ? "" : ", best model score " + std::to_string(top_.front().score)));
    logLine("resist tables: " + std::to_string(resistTableCount_.load()) + " (" +
            std::to_string(resistExactTables_.load()) + " evaluated in full), " + std::to_string(resistEvals_.load()) +
            " sets evaluated, " + std::to_string(resistViolations_.load()) + " bound violations; built in " +
            std::to_string(resistBoundNs_.load() / 1e9) + " + " + std::to_string(resistSortNs_.load() / 1e9) +
            " s thread time (bounds + sorting)" +
            (resistVerify_ ? " (all sets verified)" : ""));
    if (resistViolations_.load())
        result_.warnings.push_back("resist set lower bounds violated " + std::to_string(resistViolations_.load()) +
                                   " times: stage 1 order may be inexact");
}
}  // namespace

namespace
{
void Search::stage2()
{
    using Memo = std::unordered_map<std::string, Scored>;
    using Final = FinalSet;
    // One starting set's local search.
    struct Start
    {
        Scored first, best;  // the starting set, and where the search ended
        std::vector<int> ids;
        FitInput fit;
        Memo memo;  // its own evaluations
        size_t evaluations = 0;
        bool capped = false;  // hit kStage2MaxMoves (should never happen)
    };
    const int budget = kPlanItemBudget - 2 * static_cast<int>(base_.modules.size()) - 4;
    auto search = [&](const Found& f, const Memo& shared, Start& out) {
        // The set's modules, one slot each, in rack order.
        std::vector<int> slotKinds;
        std::vector<uint8_t> counts(kinds_.size(), 0);
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            const Multiset& m = coreMs_[ri][f.ms[ri]];
            for (int i = 0; i < m.n; ++i)
                ++counts[static_cast<size_t>(coreKinds_[ri][m.k[static_cast<size_t>(i)]])];
            const Multiset& rm = resistMs_[ri][resistSets_[static_cast<size_t>(f.rset)].ms[ri]];
            for (int i = 0; i < rm.n; ++i)
                ++counts[static_cast<size_t>(resistKinds_[ri][rm.k[static_cast<size_t>(i)]])];
            const Multiset& fm = fitMs_[ri][fitSets_[static_cast<size_t>(f.fset)].ms[ri]];
            for (int i = 0; i < fm.n; ++i)
                ++counts[static_cast<size_t>(fitKinds_[ri][fm.k[static_cast<size_t>(i)]])];
        }
        std::array<int, RackCount> used{};
        for (int r = 0; r < RackCount; ++r)
            for (size_t k = 0; k < kinds_.size(); ++k)
                if (kinds_[k].rack == r)
                    for (int i = 0; i < counts[k]; ++i)
                    {
                        slotKinds.push_back(static_cast<int>(k));
                        ++used[static_cast<size_t>(r)];
                    }
        for (int r = 0; r < RackCount; ++r)
            if (used[static_cast<size_t>(r)] > slots_[static_cast<size_t>(r)])
                throw std::logic_error(std::string("stage 1 produced too many ") + rackName(r) + " modules");
        // Options per slot: the subgroup's members, strongest first (trimmed to fit a plan).
        std::vector<std::vector<int>> options;
        int items = 0;
        for (int k : slotKinds)
        {
            options.push_back(subgroups_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].subgroup)].members);
            for (int c : options.back())
                items += cands_[static_cast<size_t>(c)].input.chargeID ? 2 : 1;
        }
        while (items > budget)
        {
            size_t widest = 0;
            for (size_t i = 1; i < options.size(); ++i)
                if (options[i].size() > options[widest].size())
                    widest = i;
            if (options[widest].size() <= 1)
                break;
            items -= cands_[static_cast<size_t>(options[widest].back())].input.chargeID ? 2 : 1;
            options[widest].pop_back();
        }
        SearchSpec spec;
        spec.base = base_;
        for (const auto& opts : options)
        {
            std::vector<ModuleInput> in;
            for (int c : opts)
                in.push_back(cands_[static_cast<size_t>(c)].input);
            spec.slots.push_back(std::move(in));
        }
        SearchPlan plan(engine_, spec);

        std::vector<int> choice(options.size(), 0);
        auto evaluate = [&](const std::vector<int>& ch) {
            std::vector<int> ids;
            for (size_t i = 0; i < ch.size(); ++i)
                ids.push_back(options[i][static_cast<size_t>(ch[i])]);
            std::sort(ids.begin(), ids.end());
            std::string key(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(int));
            if (auto it = out.memo.find(key); it != out.memo.end())
                return it->second;
            if (auto it = shared.find(key); it != shared.end())
                return it->second;
            Scored s;
            // Group and type limits (pyfa: the raw maxGroupFitted of each module).
            std::unordered_map<int, int> groupN = pinnedGroupCount_, typeN = pinnedTypeCount_;
            for (int c : ids)
            {
                ++groupN[cands_[static_cast<size_t>(c)].groupID];
                ++typeN[cands_[static_cast<size_t>(c)].type->id];
            }
            s.valid = true;
            for (int c : ids)
            {
                const Candidate& cd = cands_[static_cast<size_t>(c)];
                if ((cd.maxGroup && groupN[cd.groupID] > cd.maxGroup) || (cd.maxType && typeN[cd.type->id] > cd.maxType))
                    s.valid = false;
                s.price += cd.price;
            }
            if (s.valid)
            {
                s.r = plan.evaluate(ch.data(), goal_);
                ++out.evaluations;
                s.goal = s.r.goal;
                s.bucket = goalBucket(s.goal);
                s.fits = fitsOf(s.r);
            }
            out.memo.emplace(key, s);
            return s;
        };

        Scored cur = evaluate(choice);
        out.first = cur;
        // Every accepted move strictly improves `better`'s key (a strict weak order),
        // so no set is visited twice and the search ends. The cap is a backstop only.
        size_t moves = 0;
        for (bool improved = true; improved;)
        {
            if (moves >= kStage2MaxMoves)
            {
                out.capped = true;
                break;
            }
            improved = false;
            for (size_t i = 0; i < choice.size(); ++i)
                for (int o = 0; o < static_cast<int>(options[i].size()); ++o)
                {
                    if (o == choice[i])
                        continue;
                    auto next = choice;
                    next[i] = o;
                    Scored s = evaluate(next);
                    if (better(s, cur))
                        cur = s, choice = next, improved = true, ++moves;
                }
            if (improved)
                continue;
            for (size_t i = 0; i < choice.size() && !improved; ++i)
                for (size_t j = i + 1; j < choice.size() && !improved; ++j)
                    for (int oi = 0; oi < static_cast<int>(options[i].size()) && !improved; ++oi)
                        for (int oj = 0; oj < static_cast<int>(options[j].size()); ++oj)
                        {
                            if (oi == choice[i] && oj == choice[j])
                                continue;
                            auto next = choice;
                            next[i] = oi;
                            next[j] = oj;
                            Scored s = evaluate(next);
                            if (better(s, cur))
                            {
                                cur = s, choice = next, improved = true, ++moves;
                                break;
                            }
                        }
        }
        out.best = cur;
        for (size_t i = 0; i < choice.size(); ++i)
            out.ids.push_back(options[i][static_cast<size_t>(choice[i])]);
        out.fit = plan.fitFor(choice.data());
    };

    // Starting sets in score order, kBatch at a time in parallel; each sees the
    // memo of the earlier batches. The results are then taken in order and the
    // patience rule applied, so the batch (not the thread count) fixes the result.
    constexpr size_t kBatch = 16;
    Memo memo;
    std::map<std::string, Final> finals;
    Scored bestFit;
    int sinceBest = 0;
    bool done = false;
    size_t cappedStarts = 0;
    std::vector<Start> starts;
    for (size_t first = 0; first < top_.size() && !done; first += kBatch)
    {
        const size_t n = std::min(kBatch, top_.size() - first);
        starts.assign(n, Start{});
        if (opt_.progress && first > 0)
            opt_.progress(0.7 + (opt_.refine ? 0.1 : 0.3) * std::min(1.0, static_cast<double>(first) /
                                                        static_cast<double>(std::min<size_t>(top_.size(), 4 * static_cast<size_t>(opt_.patience)))),
                          "stage 2");
        pool_->run(n, 1, [&](size_t i, int) { search(top_[first + i], memo, starts[i]); });
        for (size_t i = 0; i < n && !done; ++i)
        {
            Start& st = starts[i];
            const Found& f = top_[first + i];
            ++result_.stats.stage2Starts;
            result_.stats.stage2Evaluations += st.evaluations;
            if (st.capped)
                ++cappedStarts;
            for (auto& [k, v] : st.memo)
                memo.emplace(k, v);
            if (st.first.valid && f.score > 0)
                maxModelError_ = std::max(maxModelError_, std::abs(st.first.goal - f.score) / f.score);
            const Scored& cur = st.best;
            std::vector<int> sortedIds = st.ids;
            std::sort(sortedIds.begin(), sortedIds.end());
            std::string key(reinterpret_cast<const char*>(sortedIds.data()), sortedIds.size() * sizeof(int));
            if (cur.valid && !finals.count(key))
                finals[key] = {cur, st.ids, std::move(st.fit)};
            if (cur.valid && cur.fits && better(cur, bestFit))
            {
                bestFit = cur;
                sinceBest = 0;
            }
            else if (bestFit.fits && ++sinceBest >= opt_.patience)
                done = true;
        }
    }


    if (cappedStarts)
    {
        const std::string w = "stage 2: local search stopped at the move cap (" + std::to_string(kStage2MaxMoves) +
                              ") on " + std::to_string(cappedStarts) + " starting sets";
        logLine("warning: " + w);
        result_.warnings.push_back(w);
    }

    // Every distinct end set (in key order), for refine() and the report.
    finals_.clear();
    for (auto& [k, v] : finals)
        finals_.push_back({v.s, v.cands, std::move(v.fit)});
    if (bestFit.fits)
        result_.stats.stage2Best = bestFit.goal;
    logLine("stage 2: " + std::to_string(result_.stats.stage2Starts) + " starting sets, " +
            std::to_string(result_.stats.stage2Evaluations) + " exact evaluations; model vs exact max relative error " +
            std::to_string(maxModelError_));
}

// The best fitting sets of finals_ (stage 2's, then refine's): only fitting sets are
// reported, a set that doesn't fit can't be applied. When none fits, run() reports
// the request as infeasible.
void Search::report()
{
    std::vector<const FinalSet*> order;
    for (const FinalSet& f : finals_)
        if (f.s.valid && f.s.fits)
            order.push_back(&f);
    // Stable over finals_' order (stage 2's key order, then refine's finds), so ties stay deterministic.
    std::stable_sort(order.begin(), order.end(), [](const FinalSet* a, const FinalSet* b) { return better(a->s, b->s); });
    result_.fits.clear();
    for (const FinalSet* fin : order)
    {
        if (static_cast<int>(result_.fits.size()) >= opt_.results)
            break;
        FoundFit ff;
        ff.fit = fin->fit;
        ff.exact = fin->s.r;
        ff.fits = fin->s.fits;
        ff.price = fin->s.price;
        for (int c : fin->cands)
        {
            const Candidate& cd = cands_[static_cast<size_t>(c)];
            ff.typeIDs[cd.rack].push_back(cd.type->id);
            ff.modules[cd.rack].push_back(cd.input);
        }
        result_.fits.push_back(std::move(ff));
    }
}

// ---- refine
// Stage 1 sees one representative per subgroup, and stage 2 swaps members only within
// a slot's subgroup. Refine searches every module choice of each open rack: every
// useful member of every subgroup (any role) and the other charges of charge-using
// modules (measure() keeps the best charge only), less the dominated ones (buildPool).
// Moves, each evaluated exactly with a SearchPlan built for it:
// - one position (a fitted module, or an empty slot) to any choice of its rack, or empty;
// - two positions at once: the pairs whose one-move effects, added up, promise the
//   best goal and to fit are evaluated exactly (Options::refinePairs per test). This
//   finds trades a single move can't make, e.g. a cheaper repairer that frees the CPU
//   for a stronger hardener.
// A set's score is its canonical evaluation (Engine::evaluate of the pinned modules
// followed by the set in rack and candidate order), memoized by set, so `better` is a
// strict weak order on sets and every accepted move strictly improves it. Moves are
// screened with their plan's evaluation and accepted on the canonical one.
//
// Phase 1 descends from stage 2's best end sets (in parallel; from the empty set when
// none fits). Phase 2 is an iterated local search on Options::refineChains chains
// (16): each round perturbs every chain's set, descends again and keeps the result if
// it is better. Half the perturbations kick in one of the strongest one-move
// neighbours that didn't improve the set (as a rule an upgrade that doesn't fit, which
// the descent then pays for elsewhere) plus 0-1 random changes; the others change 1-3
// random positions to random choices. Rounds run until the time budget
// (Options::timeBudget, whole run) is used, Options::refinePatience chain iterations
// bring no better best set, or Options::refineRounds is reached. Each chain's random
// numbers depend only on its index and the round, and rounds are synchronous, so with
// a fixed chain count the result doesn't depend on the thread count or timing unless
// the time budget ends the run.

void Search::buildPool()
{
    for (auto& p : choices_)
        p.clear();
    for (auto& p : allChoices_)
        p.clear();
    // Per subgroup: its useful members, then the other charges of its charge modules.
    std::vector<std::vector<int>> bySub(subgroups_.size());
    std::unordered_map<int, int> subOfType;
    for (size_t g = 0; g < subgroups_.size(); ++g)
        for (int m : subgroups_[g].members)
        {
            bySub[g].push_back(m);
            subOfType.emplace(cands_[static_cast<size_t>(m)].type->id, static_cast<int>(g));
        }
    // OPT_NO_CHARGE_ALTS (study): only the best charge of each charge-using module.
    const bool noAlts = [] {
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, "OPT_NO_CHARGE_ALTS") == 0 && e;
        free(e);
        return on;
    }();
    std::vector<bool> isAlt(cands_.size(), false);
    for (Candidate& c : chargeAlts_)
    {
        auto g = subOfType.find(c.input.typeID);
        if (noAlts || g == subOfType.end() || slots_[static_cast<size_t>(c.rack)] <= 0)
            continue;
        c.useful = true;
        c.subgroup = g->second;
        bySub[static_cast<size_t>(g->second)].push_back(static_cast<int>(cands_.size()));
        cands_.push_back(std::move(c));
        isAlt.push_back(true);
    }
    chargeAlts_.clear();

    // Dominated choices go: another choice of the same subgroup (same group, rack, resists
    // changed and size) that is at least as strong in every measured effect and costs at
    // most as much CPU, power, calibration, capacitor and ISK, with no tighter fitting
    // limit, is at least as good in every set. (Charge alternatives aren't measured
    // against other modules: in a subgroup that modifies others they are kept.)
    // OPT_NO_PRUNE (study) keeps every choice.
    const bool noPrune = [] {
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, "OPT_NO_PRUNE") == 0 && e;
        free(e);
        return on;
    }();
    auto gains = [&](const Candidate& c) {
        return std::array<double, 15>{c.goalGain,      c.dividerDrop,    c.hpGain,         c.capGain,
                                      c.outputGain,    c.rep,            c.pairGain,       c.costGain,
                                      c.outputDelta[0], c.outputDelta[1], c.outputDelta[2], c.pinnedDrop[0],
                                      c.pinnedDrop[1], c.pinnedDrop[2],  -c.capUse};
    };
    auto costs = [&](const Candidate& c) { return std::array<double, 4>{c.cpu, c.power, c.calibration, c.price}; };
    auto limitOk = [](int a, int b) { return a == 0 || (b != 0 && a >= b); };  // a's limit no tighter than b's
    size_t pruned = 0;
    for (size_t g = 0; g < bySub.size(); ++g)
    {
        const auto& list = bySub[g];
        bool influences = false;
        for (int m : list)
            influences |= cands_[static_cast<size_t>(m)].pairGain > 0 || cands_[static_cast<size_t>(m)].costGain > 0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Candidate& b = cands_[static_cast<size_t>(list[i])];
            allChoices_[static_cast<size_t>(b.rack)].push_back(list[i]);
            bool dominated = false;
            for (size_t j = 0; j < list.size() && !dominated && !noPrune; ++j)
            {
                if (i == j)
                    continue;
                const Candidate& a = cands_[static_cast<size_t>(list[j])];
                if (influences && (isAlt[static_cast<size_t>(list[i])] || isAlt[static_cast<size_t>(list[j])]))
                    continue;
                if (!limitOk(a.maxType, b.maxType) || !limitOk(a.maxGroup, b.maxGroup))
                    continue;
                const auto ga = gains(a), gb = gains(b);
                const auto ca = costs(a), cb = costs(b);
                bool ge = true, strict = false;
                for (size_t k = 0; k < ga.size() && ge; ++k)
                    ge = ga[k] >= gb[k], strict |= ga[k] > gb[k];
                for (size_t k = 0; k < ca.size() && ge; ++k)
                    ge = ca[k] <= cb[k], strict |= ca[k] < cb[k];
                // Equal in everything: the earlier one stays.
                dominated = ge && (strict || j < i);
            }
            if (dominated)
                ++pruned;
            else
                choices_[static_cast<size_t>(b.rack)].push_back(list[i]);
        }
    }
    logLine("refine: " + std::to_string(choices_[RackMid].size()) + " mid, " + std::to_string(choices_[RackLow].size()) +
            " low, " + std::to_string(choices_[RackRig].size()) + " rig choices (" + std::to_string(pruned) +
            " dominated dropped)");
}

std::string Search::keyOf(const RSet& x) const
{
    std::vector<int> ids;
    for (const auto& v : x.m)
        ids.insert(ids.end(), v.begin(), v.end());
    std::sort(ids.begin(), ids.end());
    return std::string(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(int));
}

// Group and type limits (pyfa: the raw maxGroupFitted / maxTypeFitted of each
// module), the pinned modules counted.
bool Search::validIds(const std::vector<int>& ids) const
{
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const Candidate& c = cands_[static_cast<size_t>(ids[i])];
        if (!c.maxGroup && !c.maxType)
            continue;
        int g = 0, t = 0;
        if (auto it = pinnedGroupCount_.find(c.groupID); it != pinnedGroupCount_.end())
            g = it->second;
        if (auto it = pinnedTypeCount_.find(c.type->id); it != pinnedTypeCount_.end())
            t = it->second;
        for (int j : ids)
        {
            const Candidate& d = cands_[static_cast<size_t>(j)];
            g += d.groupID == c.groupID;
            t += d.type->id == c.type->id;
        }
        if ((c.maxGroup && g > c.maxGroup) || (c.maxType && t > c.maxType))
            return false;
    }
    return true;
}

bool Search::validSet(const RSet& x) const
{
    std::vector<int> ids;
    for (int r = 0; r < RackCount; ++r)
    {
        const auto& v = x.m[static_cast<size_t>(r)];
        if (static_cast<int>(v.size()) > slots_[static_cast<size_t>(r)])
            return false;
        ids.insert(ids.end(), v.begin(), v.end());
    }
    return validIds(ids);
}

FitInput Search::fitOf(const RSet& x) const
{
    FitInput f = base_;
    for (const auto& v : x.m)
        for (int c : v)
            f.modules.push_back(cands_[static_cast<size_t>(c)].input);
    return f;
}

double Search::priceOf(const RSet& x) const
{
    double p = 0;
    for (const auto& v : x.m)
        for (int c : v)
            p += cands_[static_cast<size_t>(c)].price;
    return p;
}

Scored Search::canonical(RSet& x)
{
    const std::string key = keyOf(x);
    {
        std::lock_guard lock(canonM_);
        if (auto it = canonMemo_.find(key); it != canonMemo_.end())
            return x.s = it->second;
    }
    Scored s;
    s.valid = validSet(x);
    s.price = priceOf(x);
    if (s.valid)
    {
        s.r = toTestResult(engine_.evaluate(fitOf(x)), goal_);
        s.goal = s.r.goal;
        s.bucket = goalBucket(s.goal);
        s.fits = fitsOf(s.r);
        refineEvals_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(canonM_);
        canonMemo_.emplace(key, s);
    }
    return x.s = s;
}

namespace
{
// Plan items a module takes (itself and its charge).
int itemsOf(const ModuleInput& m) { return m.chargeID ? 2 : 1; }
int itemsOf(const FitInput& f)
{
    int n = 0;
    for (const ModuleInput& m : f.modules)
        n += itemsOf(m);
    return n;
}
}  // namespace

// One step of the local search on x: the best one-move neighbour that improves x,
// else (with `pairs`) the best verified pair move. Returns whether x changed.
bool Search::improveOnce(RSet& x, RefineStats& st, bool pairs)
{
    ++st.sweeps;
    struct Pos
    {
        int rack, out;  // out: the fitted module (cands_ index) it replaces, -1 for an empty slot
    };
    struct Nb
    {
        int pos = 0, in = -1;  // in: the choice put there (-1: the slot is emptied)
        int pos2 = -1, in2 = -1;
        Scored s;  // as evaluated in the move's plan (screening)
    };
    std::vector<Pos> pos;
    for (int r = 0; r < RackCount; ++r)
    {
        const size_t ri = static_cast<size_t>(r);
        if (!slots_[ri])
            continue;
        const auto& v = x.m[ri];
        for (size_t i = 0; i < v.size(); ++i)
            if (i == 0 || v[i] != v[i - 1])
                pos.push_back({r, v[i]});
        if (static_cast<int>(v.size()) < slots_[ri])
            pos.push_back({r, -1});
    }
    auto without = [&](const RSet& s, int rack, int out) {
        RSet y = s;
        if (out >= 0)
        {
            auto& v = y.m[static_cast<size_t>(rack)];
            v.erase(std::find(v.begin(), v.end(), out));
        }
        return y;
    };
    auto withIn = [&](RSet& y, int rack, int in) {
        if (in < 0)
            return;
        auto& v = y.m[static_cast<size_t>(rack)];
        v.insert(std::upper_bound(v.begin(), v.end(), in), in);
    };
    auto allIds = [&](const RSet& y) {
        std::vector<int> ids;
        for (const auto& v : y.m)
            ids.insert(ids.end(), v.begin(), v.end());
        return ids;
    };
    auto score = [&](const TestResult& r, double price) {
        Scored s;
        s.valid = true;
        s.r = r;
        s.goal = r.goal;
        s.bucket = goalBucket(r.goal);
        s.fits = fitsOf(r);
        s.price = price;
        return s;
    };
    // Tries the screened moves best first: the first whose canonical evaluation beats x wins.
    auto accept = [&](std::vector<Nb>& moves) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < moves.size(); ++i)
            if (better(moves[i].s, x.s))
                idx.push_back(i);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return better(moves[a].s, moves[b].s); });
        constexpr size_t kTries = 4;
        for (size_t t = 0; t < idx.size() && t < kTries; ++t)
        {
            const Nb& n = moves[idx[t]];
            const Pos& p = pos[static_cast<size_t>(n.pos)];
            RSet y = without(x, p.rack, p.out);
            if (n.pos2 >= 0)
            {
                const Pos& q = pos[static_cast<size_t>(n.pos2)];
                y = without(y, q.rack, q.out);
                withIn(y, q.rack, n.in2);
            }
            withIn(y, p.rack, n.in);
            auto tc = Clock::now();
            canonical(y);
            st.canonicalSeconds += std::chrono::duration<double>(Clock::now() - tc).count();
            ++st.canonicalEvals;
            if (better(y.s, x.s))
            {
                x = std::move(y);
                return true;
            }
        }
        return false;
    };

    // One-move neighbours: per position, one plan (chunked) over the rack's choices.
    std::vector<Nb> nbs;
    for (size_t p = 0; p < pos.size(); ++p)
    {
        const int r = pos[p].rack, out = pos[p].out;
        if (out < 0 && choices_[static_cast<size_t>(r)].empty())
            continue;
        const RSet rest = without(x, r, out);
        const double restPrice = priceOf(rest);
        std::vector<int> ids = allIds(rest);
        std::vector<int> opts;
        for (int c : choices_[static_cast<size_t>(r)])
        {
            if (c == out)
                continue;
            ids.push_back(c);
            if (validIds(ids))
                opts.push_back(c);
            ids.pop_back();
        }
        SearchSpec spec;
        spec.base = fitOf(rest);
        const int budget = kPlanItemBudget - 2 * static_cast<int>(spec.base.modules.size()) - 4;
        bool first = true;
        for (size_t at = 0; at < opts.size() || first;)
        {
            size_t end = at;
            int items = 0;
            std::vector<ModuleInput> in;
            while (end < opts.size() && items + 2 <= budget)
            {
                in.push_back(cands_[static_cast<size_t>(opts[end])].input);
                items += itemsOf(in.back());
                ++end;
            }
            spec.slots = {std::move(in)};
            auto tb = Clock::now();
            SearchPlan plan(engine_, spec);
            auto te = Clock::now();
            st.planSeconds += std::chrono::duration<double>(te - tb).count();
            ++st.plans;
            if (first && out >= 0)
            {
                int none = -1;
                Nb n;
                n.pos = static_cast<int>(p);
                n.s = score(plan.evaluate(&none, goal_), restPrice);
                nbs.push_back(n);
                ++st.evaluations;
            }
            first = false;
            for (size_t i = at; i < end; ++i)
            {
                int choice = static_cast<int>(i - at);
                Nb n;
                n.pos = static_cast<int>(p);
                n.in = opts[i];
                n.s = score(plan.evaluate(&choice, goal_), restPrice + cands_[static_cast<size_t>(opts[i])].price);
                nbs.push_back(n);
                ++st.evaluations;
            }
            st.evalSeconds += std::chrono::duration<double>(Clock::now() - te).count();
            at = end;
        }
    }
    if (accept(nbs))
        return true;
    {
        // Kicks for the iterated search: the strongest neighbours by goal alone.
        std::vector<size_t> idx;
        for (size_t i = 0; i < nbs.size(); ++i)
            if (nbs[i].in >= 0 && nbs[i].s.goal > x.s.goal)
                idx.push_back(i);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return nbs[a].s.goal > nbs[b].s.goal; });
        x.kicks.clear();
        for (size_t t = 0; t < idx.size() && t < 32; ++t)
        {
            const Pos& p = pos[static_cast<size_t>(nbs[idx[t]].pos)];
            x.kicks.push_back({p.rack, p.out, nbs[idx[t]].in});
        }
    }
    if (!pairs || nbs.empty())
        return false;

    // Pair moves: the one-move effects (against x) added up. The pairs with the best
    // estimated goal that are estimated to fit (0.5 % slack: costs interact, e.g. rigs
    // that lower module costs) are evaluated exactly.
    ++st.pairChecks;
    const TestResult& r0 = x.s.r;
    const double used0[3] = {r0.cpuUsed, r0.powerUsed, r0.calibrationUsed};
    const double out0[3] = {r0.cpuOutput, r0.powerOutput, r0.calibrationOutput};
    struct Delta
    {
        double goal;
        double used[3], out[3];
    };
    std::vector<Delta> d(nbs.size());
    for (size_t i = 0; i < nbs.size(); ++i)
    {
        const TestResult& r = nbs[i].s.r;
        d[i].goal = r.goal - r0.goal;
        const double used[3] = {r.cpuUsed, r.powerUsed, r.calibrationUsed};
        const double out[3] = {r.cpuOutput, r.powerOutput, r.calibrationOutput};
        for (size_t k = 0; k < 3; ++k)
        {
            d[i].used[k] = used[k] - used0[k];
            d[i].out[k] = out[k] - out0[k];
        }
    }
    std::vector<size_t> byGain(nbs.size());
    std::iota(byGain.begin(), byGain.end(), size_t{0});
    std::stable_sort(byGain.begin(), byGain.end(), [&](size_t a, size_t b) { return d[a].goal > d[b].goal; });
    auto multiplicity = [&](const Pos& p) {
        const auto& v = x.m[static_cast<size_t>(p.rack)];
        if (p.out < 0)
            return slots_[static_cast<size_t>(p.rack)] - static_cast<int>(v.size());
        return static_cast<int>(std::count(v.begin(), v.end(), p.out));
    };
    struct Pair
    {
        double est;
        size_t a, b;  // byGain ranks
    };
    auto worse = [](const Pair& a, const Pair& b) { return a.est != b.est ? a.est > b.est : std::make_pair(a.a, a.b) < std::make_pair(b.a, b.b); };
    std::vector<Pair> heap;  // min-heap by est (the worst kept pair on top)
    const size_t N = static_cast<size_t>(std::max(1, opt_.refinePairs));
    const double floorGain = x.s.fits ? std::max(1e-7 * std::abs(r0.goal), 1e-9) : -std::numeric_limits<double>::infinity();
    for (size_t ia = 0; ia < byGain.size(); ++ia)
    {
        const size_t a = byGain[ia];
        for (size_t ib = ia + 1; ib < byGain.size(); ++ib)
        {
            const size_t b = byGain[ib];
            const double gain = d[a].goal + d[b].goal;
            const double thr = heap.size() >= N ? std::max(floorGain, heap.front().est) : floorGain;
            if (!(gain > thr))
                break;
            if (nbs[a].pos == nbs[b].pos && multiplicity(pos[static_cast<size_t>(nbs[a].pos)]) < 2)
                continue;
            bool fit = true;
            for (size_t k = 0; k < 3 && fit; ++k)
                fit = used0[k] + d[a].used[k] + d[b].used[k] <= (out0[k] + d[a].out[k] + d[b].out[k]) * 1.005 + 1e-6;
            if (!fit)
                continue;
            heap.push_back({gain, ia, ib});
            std::push_heap(heap.begin(), heap.end(), worse);
            if (heap.size() > N)
            {
                std::pop_heap(heap.begin(), heap.end(), worse);
                heap.pop_back();
            }
        }
    }
    if (heap.empty())
        return false;
    std::sort(heap.begin(), heap.end(), [&](const Pair& p, const Pair& q) { return worse(p, q); });
    // Exact evaluation, one plan per position pair.
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> byPos;  // (pos, pos2) -> (in, in2)
    for (const Pair& pr : heap)
    {
        size_t a = byGain[pr.a], b = byGain[pr.b];
        if (nbs[a].pos > nbs[b].pos || (nbs[a].pos == nbs[b].pos && nbs[a].in > nbs[b].in))
            std::swap(a, b);
        byPos[{nbs[a].pos, nbs[b].pos}].push_back({nbs[a].in, nbs[b].in});
    }
    std::vector<Nb> moves;
    for (const auto& [pp, list] : byPos)
    {
        const Pos& p = pos[static_cast<size_t>(pp.first)];
        const Pos& q = pos[static_cast<size_t>(pp.second)];
        const RSet rest = without(without(x, p.rack, p.out), q.rack, q.out);
        const double restPrice = priceOf(rest);
        std::vector<int> ins[2];
        for (const auto& [i1, i2] : list)
        {
            if (i1 >= 0 && std::find(ins[0].begin(), ins[0].end(), i1) == ins[0].end())
                ins[0].push_back(i1);
            if (i2 >= 0 && std::find(ins[1].begin(), ins[1].end(), i2) == ins[1].end())
                ins[1].push_back(i2);
        }
        SearchSpec spec;
        spec.base = fitOf(rest);
        for (const auto& v : ins)
        {
            std::vector<ModuleInput> in;
            for (int c : v)
                in.push_back(cands_[static_cast<size_t>(c)].input);
            spec.slots.push_back(std::move(in));
        }
        if (itemsOf(spec.base) + 2 * static_cast<int>(ins[0].size() + ins[1].size()) > kPlanItemBudget)
            continue;  // can't happen with refinePairs in the hundreds; skip rather than fail
        auto tb = Clock::now();
        SearchPlan plan(engine_, spec);
        auto te = Clock::now();
        st.planSeconds += std::chrono::duration<double>(te - tb).count();
        ++st.plans;
        std::vector<int> ids = allIds(rest);
        const size_t restN = ids.size();
        for (const auto& [i1, i2] : list)
        {
            ids.resize(restN);
            if (i1 >= 0)
                ids.push_back(i1);
            if (i2 >= 0)
                ids.push_back(i2);
            if (!validIds(ids))
                continue;
            int choice[2] = {i1 < 0 ? -1 : static_cast<int>(std::find(ins[0].begin(), ins[0].end(), i1) - ins[0].begin()),
                             i2 < 0 ? -1 : static_cast<int>(std::find(ins[1].begin(), ins[1].end(), i2) - ins[1].begin())};
            double price = restPrice + (i1 >= 0 ? cands_[static_cast<size_t>(i1)].price : 0) +
                           (i2 >= 0 ? cands_[static_cast<size_t>(i2)].price : 0);
            Nb n;
            n.pos = pp.first;
            n.in = i1;
            n.pos2 = pp.second;
            n.in2 = i2;
            n.s = score(plan.evaluate(choice, goal_), price);
            moves.push_back(n);
            ++st.evaluations;
        }
        st.evalSeconds += std::chrono::duration<double>(Clock::now() - te).count();
    }
    return accept(moves);
}

void Search::localSearch(RSet& x, RefineStats& st, const Clock::time_point* deadline)
{
    for (size_t moves = 0; moves < kStage2MaxMoves; ++moves)
    {
        if (deadline && Clock::now() >= *deadline)
            return;
        if (!improveOnce(x, st, opt_.refinePairs > 0))
            return;
    }
}

void Search::refine(Clock::time_point t0)
{
    Stats& stats = result_.stats;
    buildPool();
    const bool timed = opt_.timeBudget > 0;
    const Clock::time_point deadline =
        t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timed ? opt_.timeBudget : 0));
    const Clock::time_point* dl = timed ? &deadline : nullptr;
    std::vector<RefineStats> rs(static_cast<size_t>(pool_->size()) + 64);

    std::unordered_map<std::string, size_t> seen;
    auto keyOfIds = [](std::vector<int> ids) {
        std::sort(ids.begin(), ids.end());
        return std::string(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(int));
    };
    for (size_t i = 0; i < finals_.size(); ++i)
        seen.emplace(keyOfIds(finals_[i].cands), i);
    auto toRSet = [&](const std::vector<int>& cands) {
        RSet x;
        for (int c : cands)
            x.m[static_cast<size_t>(cands_[static_cast<size_t>(c)].rack)].push_back(c);
        for (auto& v : x.m)
            std::sort(v.begin(), v.end());
        return x;
    };
    Scored best;  // best fitting set so far
    for (const FinalSet& f : finals_)
        if (f.s.valid && f.s.fits && better(f.s, best))
            best = f.s;
    auto add = [&](const RSet& x) {
        if (!x.s.valid)
            return false;
        std::vector<int> ids;
        for (const auto& v : x.m)
            ids.insert(ids.end(), v.begin(), v.end());
        if (seen.emplace(keyOfIds(ids), finals_.size()).second)
            finals_.push_back({x.s, ids, fitOf(x)});
        if (x.s.fits && better(x.s, best))
        {
            best = x.s;
            return true;
        }
        return false;
    };

    // Phase 1: descend from stage 2's best distinct end sets (and the empty set when
    // none fits).
    const size_t chains = static_cast<size_t>(opt_.refineChains > 0 ? opt_.refineChains : pool_->size());
    std::vector<size_t> order;
    for (size_t i = 0; i < finals_.size(); ++i)
        if (finals_[i].s.valid)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return better(finals_[a].s, finals_[b].s); });
    const size_t nStarts =
        std::min(order.size(), opt_.refineStarts > 0 ? static_cast<size_t>(opt_.refineStarts) : std::max<size_t>(chains, 8));
    // OPT_REFINE_FROM_EMPTY (study): start from the empty set only, not from stage 2's sets.
    const bool fromEmpty = [] {
        char* e = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&e, &n, "OPT_REFINE_FROM_EMPTY") == 0 && e;
        free(e);
        return on;
    }();
    std::vector<RSet> starts;
    for (size_t i = 0; i < nStarts && !fromEmpty; ++i)
        starts.push_back(toRSet(finals_[order[i]].cands));
    if (!best.fits || fromEmpty)
        starts.push_back(RSet{});
    // Small requests: every combination of the choices, exactly, when there are at most
    // Options::refineExact; its best set is a start (and an end set) too.
    if (opt_.refineExact > 0 && !fromEmpty)
    {
        // Plan builds (~1-2 ms each) cost more than the evaluations beyond ~20k plans.
        const Enumerated e = enumerate(choices_, opt_.refineExact, 20000);
        if (e.ran)
        {
            logLine("refine: exact pass over " + std::to_string(e.combinations) + " combinations (" +
                    std::to_string(e.plans) + " plans) in " + std::to_string(e.seconds) + " s, best " +
                    (e.best.fits ? std::to_string(e.best.goal) : std::string("none")));
            stats.refineExact = e.combinations;
            if (e.best.fits)
            {
                RSet x = toRSet(e.ids);
                canonical(x);
                starts.insert(starts.begin(), x);
            }
        }
    }
    stats.refineStarts = starts.size();
    pool_->run(starts.size(), 1, [&](size_t i, int w) {
        canonical(starts[i]);
        localSearch(starts[i], rs[static_cast<size_t>(w)], dl);
    });
    for (const RSet& x : starts)
        stats.refineImproved += add(x) ? 1 : 0;
    logLine("refine: " + std::to_string(starts.size()) + " starts descended" +
            (best.fits ? ", best " + std::to_string(best.goal) : std::string(", none fits")) + " (" +
            std::to_string(secondsSince(t0)) + " s)");

    // Phase 2: iterated local search, one chain per thread.
    std::vector<RSet> chain;
    {
        std::vector<const RSet*> locals;
        for (const RSet& x : starts)
            if (x.s.valid)
                locals.push_back(&x);
        std::stable_sort(locals.begin(), locals.end(), [&](const RSet* a, const RSet* b) { return better(a->s, b->s); });
        for (size_t c = 0; c < chains && !locals.empty(); ++c)
            chain.push_back(*locals[c % locals.size()]);
    }
    const int totalSlots = slots_[RackMid] + slots_[RackLow] + slots_[RackRig];
    // OPT_KICK (study): percentage of rounds that start with a guided kick (default 50).
    const int guidedPct = [] {
        char* e = nullptr;
        size_t n = 0;
        int v = 50;
        if (_dupenv_s(&e, &n, "OPT_KICK") == 0 && e)
            v = std::atoi(e);
        free(e);
        return v;
    }();
    // OPT_ACCEPT (study): percentage of worse fitting local optima a chain moves to.
    const int acceptPct = [] {
        char* e = nullptr;
        size_t n = 0;
        int v = 0;
        if (_dupenv_s(&e, &n, "OPT_ACCEPT") == 0 && e)
            v = std::atoi(e);
        free(e);
        return v;
    }();
    auto perturb = [&](RSet x, std::mt19937_64& rng) {
        int k = 1 + static_cast<int>(rng() % static_cast<uint64_t>(std::min(3, totalSlots)));
        if (!x.kicks.empty() && static_cast<int>(rng() % 100) < guidedPct)
        {
            // Guided: one of the strongest non-improving one-move neighbours (an upgrade
            // that doesn't fit, as a rule); the local search then repairs the fitting.
            const size_t n = x.kicks.size();
            const size_t t = static_cast<size_t>(std::min(rng() % n, rng() % n));  // biased to the strongest
            const auto [r, out, in] = x.kicks[t];
            RSet y = x;
            auto& v = y.m[static_cast<size_t>(r)];
            auto it = out >= 0 ? std::find(v.begin(), v.end(), out) : v.end();
            if ((out < 0 || it != v.end()) && (out >= 0 || static_cast<int>(v.size()) < slots_[static_cast<size_t>(r)]))
            {
                if (out >= 0)
                    v.erase(it);
                v.insert(std::upper_bound(v.begin(), v.end(), in), in);
                if (validSet(y))
                {
                    x = std::move(y);
                    k = static_cast<int>(rng() % 2);  // plus 0 or 1 random change
                }
            }
        }
        for (int step = 0, tries = 0; step < k && tries < 50; ++tries)
        {
            int at = static_cast<int>(rng() % static_cast<uint64_t>(totalSlots));
            int r = 0;
            while (at >= slots_[static_cast<size_t>(r)])
                at -= slots_[static_cast<size_t>(r++)];
            const auto& pool = choices_[static_cast<size_t>(r)];
            const size_t pick = static_cast<size_t>(rng() % (pool.size() + 1));
            const int in = pick < pool.size() ? pool[pick] : -1;
            RSet y = x;
            auto& v = y.m[static_cast<size_t>(r)];
            if (at < static_cast<int>(v.size()))
                v.erase(v.begin() + at);
            if (in >= 0)
                v.push_back(in);
            std::sort(v.begin(), v.end());
            if (keyOf(y) == keyOf(x) || !validSet(y))
                continue;
            x = std::move(y);
            ++step;
        }
        return x;
    };
    size_t rounds = 0;
    size_t since = 0;  // chain iterations since the best set last improved
    std::string stop = chain.empty() ? "no start" : "";
    while (stop.empty())
    {
        if (opt_.refineRounds >= 0 && rounds >= static_cast<size_t>(opt_.refineRounds))
            stop = "rounds";
        else if (since >= static_cast<size_t>(std::max(1, opt_.refinePatience)))
            stop = "converged";
        else if (timed && Clock::now() >= deadline)
            stop = "time";
        if (!stop.empty())
            break;
        std::vector<RSet> trial(chain.size());
        std::vector<int> draw(chain.size(), 100);
        pool_->run(chain.size(), 1, [&](size_t c, int w) {
            std::mt19937_64 rng(0x9E3779B97F4A7C15ull * (c + 1) ^ (0xD1B54A32D192ED03ull * (rounds + 1)));
            trial[c] = perturb(chain[c], rng);
            canonical(trial[c]);
            localSearch(trial[c], rs[static_cast<size_t>(w)], dl);
            draw[c] = static_cast<int>(rng() % 100);
        });
        bool improved = false;
        for (size_t c = 0; c < chain.size(); ++c)
        {
            if (add(trial[c]))
                improved = true, ++stats.refineImproved;
            // Better: kept. Worse but fitting: kept with probability acceptPct %, so a
            // chain can walk through worse local optima to another basin (the best set is
            // kept apart in finals_).
            if (better(trial[c].s, chain[c].s) ||
                (trial[c].s.fits && draw[c] < acceptPct && keyOf(trial[c]) != keyOf(chain[c])))
                chain[c] = std::move(trial[c]);
        }
        ++rounds;
        since = improved ? 0 : since + chain.size();
        if (opt_.progress)
        {
            double f = static_cast<double>(since) / std::max(1, opt_.refinePatience);
            if (timed)
                f = std::max(f, secondsSince(t0) / opt_.timeBudget);
            if (opt_.refineRounds > 0)
                f = std::max(f, static_cast<double>(rounds) / opt_.refineRounds);
            char phase[96];
            std::snprintf(phase, sizeof(phase), "refine: round %zu, best %.6g", rounds, best.fits ? best.goal : 0.0);
            opt_.progress(0.8 + 0.2 * std::min(1.0, f), phase);
        }
    }
    stats.refineRounds = rounds;
    stats.refineStop = stop;
    RefineStats tot;
    for (const RefineStats& s : rs)
    {
        tot.sweeps += s.sweeps;
        tot.pairChecks += s.pairChecks;
        tot.evaluations += s.evaluations;
        tot.canonicalEvals += s.canonicalEvals;
        tot.plans += s.plans;
        tot.planSeconds += s.planSeconds;
        tot.evalSeconds += s.evalSeconds;
        tot.canonicalSeconds += s.canonicalSeconds;
    }
    stats.refineEvaluations = tot.evaluations + refineEvals_.load();
    logLine("refine: " + std::to_string(rounds) + " rounds on " + std::to_string(chain.size()) + " chains (" + stop +
            "), " + std::to_string(tot.sweeps) + " sweeps, " + std::to_string(tot.pairChecks) + " pair tests, " +
            std::to_string(tot.plans) + " plans, " + std::to_string(tot.evaluations) + " plan evaluations, " +
            std::to_string(refineEvals_.load()) + " canonical; best " +
            (best.fits ? std::to_string(best.goal) : std::string("none")));
    logLine("refine thread time: plan builds " + std::to_string(tot.planSeconds) + " s (" +
            std::to_string(tot.planSeconds * 1e3 / std::max<size_t>(tot.plans, 1)) + " ms each), plan evaluations " +
            std::to_string(tot.evalSeconds) + " s (" +
            std::to_string(tot.evalSeconds * 1e6 / std::max<size_t>(tot.evaluations, 1)) + " us each), canonical " +
            std::to_string(tot.canonicalSeconds) + " s");
}

// Options::memberBruteForce: every combination of every module choice of the open
// racks (choices_), empty slots included, evaluated exactly. A rack's combinations
// are its multisets of choices (group and type limits checked). Some racks are open
// slots of one plan (all their slots); one more rack may give the plan one slot (its
// last module, in choices_ order) while its shorter prefixes go to the outer loop; the
// other racks are pinned into the plan's base, one plan per outer combination. The
// layout with the fewest plans that stays within the plan's item limit is used.
Search::Enumerated Search::enumerate(const std::array<std::vector<int>, RackCount>& lists, double limit,
                                     double maxPlans)
{
    Enumerated mc;
    const auto t0 = Clock::now();
    struct Sets
    {
        int width = 0;  // entries per multiset (-1 padded)
        size_t count = 0;
        std::vector<int> flat;
        int at(size_t set, int j) const { return flat[set * static_cast<size_t>(width) + static_cast<size_t>(j)]; }
    };
    constexpr size_t kMaxSets = 60'000'000;
    auto enumerateRack = [&](int r, int maxSize, Sets& out) {
        out = Sets{};
        out.width = std::max(0, maxSize);
        const auto& pool = lists[static_cast<size_t>(r)];
        std::vector<int> cur;
        bool over = false;
        std::function<void(size_t)> rec = [&](size_t from) {
            if (over)
                return;
            if (out.count >= kMaxSets)
            {
                over = true;
                return;
            }
            for (int j = 0; j < out.width; ++j)
                out.flat.push_back(j < static_cast<int>(cur.size()) ? cur[static_cast<size_t>(j)] : -1);
            ++out.count;
            if (static_cast<int>(cur.size()) >= maxSize)
                return;
            for (size_t i = from; i < pool.size(); ++i)
            {
                cur.push_back(pool[i]);
                if (validIds(cur))
                    rec(i);
                cur.pop_back();
            }
        };
        rec(0);
        return !over;
    };
    {
        // Upper bound (no limits): C(n + k, k) multisets of up to k of n choices per rack.
        double bound = 1;
        for (int r = 0; r < RackCount; ++r)
        {
            const double n = static_cast<double>(lists[static_cast<size_t>(r)].size());
            double c = 1;
            for (int j = 1; j <= slots_[static_cast<size_t>(r)]; ++j)
                c = c * (n + j) / j;
            bound *= c;
        }
        if (bound > 4 * limit)
        {
            mc.skipped = "too many combinations (at most " + std::to_string(bound) + ")";
            return mc;
        }
    }
    std::array<Sets, RackCount> full, prefix;
    std::array<bool, RackCount> hasPrefix{};
    double total = 1;
    for (int r = 0; r < RackCount; ++r)
    {
        const size_t ri = static_cast<size_t>(r);
        if (!enumerateRack(r, slots_[ri], full[ri]))
        {
            mc.skipped = std::string("too many ") + rackName(r) + " combinations";
            return mc;
        }
        total *= static_cast<double>(full[ri].count);
        hasPrefix[ri] = slots_[ri] > 0 && enumerateRack(r, slots_[ri] - 1, prefix[ri]);
    }
    if (total > limit)
    {
        mc.skipped = "too many combinations (" + std::to_string(total) + ")";
        return mc;
    }
    std::array<int, RackCount> slotItems{};  // items of one slot holding all of a rack's choices
    for (int r = 0; r < RackCount; ++r)
        for (int c : lists[static_cast<size_t>(r)])
            slotItems[static_cast<size_t>(r)] += itemsOf(cands_[static_cast<size_t>(c)].input);
    unsigned inner = 0;
    int split = -1;
    double bestPlans = std::numeric_limits<double>::infinity();
    for (unsigned mask = 0; mask < (1u << RackCount); ++mask)
        for (int t = -1; t < RackCount; ++t)
        {
            if (t >= 0 && ((mask >> t & 1) || !hasPrefix[static_cast<size_t>(t)]))
                continue;
            int items = 0, pinnedSlots = 0;
            double plans = 1;
            for (int r = 0; r < RackCount; ++r)
            {
                const size_t ri = static_cast<size_t>(r);
                if (mask >> r & 1)
                    items += slotItems[ri] * slots_[ri];
                else if (r == t)
                {
                    items += slotItems[ri];
                    plans *= static_cast<double>(prefix[ri].count);
                    pinnedSlots += slots_[ri] - 1;
                }
                else
                {
                    plans *= static_cast<double>(full[ri].count);
                    pinnedSlots += slots_[ri];
                }
            }
            if (items > kPlanItemBudget - 2 * (static_cast<int>(base_.modules.size()) + pinnedSlots) - 4)
                continue;
            if (plans < bestPlans)
                bestPlans = plans, inner = mask, split = t;
        }
    if (!std::isfinite(bestPlans))
    {
        mc.skipped = "no plan layout fits the item limit";
        return mc;
    }
    if (bestPlans > maxPlans)
    {
        mc.skipped = "too many plans (" + std::to_string(bestPlans) + ")";
        return mc;
    }
    const uint64_t nOuter = static_cast<uint64_t>(bestPlans + 0.5);
    const size_t lastN = split >= 0 ? lists[static_cast<size_t>(split)].size() + 1 : 1;  // + empty
    uint64_t nInner = lastN;
    for (int r = 0; r < RackCount; ++r)
        if (inner >> r & 1)
            nInner *= full[static_cast<size_t>(r)].count;
    std::vector<int> optIndex(cands_.size(), -1);
    for (const auto& pool : lists)
        for (size_t i = 0; i < pool.size(); ++i)
            optIndex[static_cast<size_t>(pool[i])] = static_cast<int>(i);

    // An outer index: the set of every outer rack (and the split rack's prefix).
    auto decodeOuter = [&](uint64_t o, std::array<size_t, RackCount>& idx) {
        for (int r = RackCount - 1; r >= 0; --r)
        {
            const size_t ri = static_cast<size_t>(r);
            if (inner >> r & 1)
                continue;
            const uint64_t n = r == split ? prefix[ri].count : full[ri].count;
            idx[ri] = static_cast<size_t>(o % n);
            o /= n;
        }
    };
    // An inner index: the set of every inner rack, and the split rack's last choice.
    auto decodeInner = [&](uint64_t i, std::array<size_t, RackCount>& idx, int& last) {
        last = static_cast<int>(i % lastN);
        i /= lastN;
        for (int r = RackCount - 1; r >= 0; --r)
        {
            const size_t ri = static_cast<size_t>(r);
            if (!(inner >> r & 1))
                continue;
            idx[ri] = static_cast<size_t>(i % full[ri].count);
            i /= full[ri].count;
        }
    };
    // The modules of a combination; false if it isn't one (the split rack's last choice
    // must not precede its prefix, empty only after an empty prefix).
    auto idsOf = [&](const std::array<size_t, RackCount>& idx, int last, std::vector<int>& ids) {
        ids.clear();
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            const Sets& s = r == split ? prefix[ri] : full[ri];
            int lastPrefix = -1;
            for (int j = 0; j < s.width; ++j)
                if (int c = s.at(idx[ri], j); c >= 0)
                    ids.push_back(c), lastPrefix = c;
            if (r != split)
                continue;
            const auto& pool = lists[ri];
            if (last == static_cast<int>(pool.size()))
            {
                if (lastPrefix >= 0)
                    return false;
            }
            else
            {
                if (lastPrefix >= 0 && last < optIndex[static_cast<size_t>(lastPrefix)])
                    return false;
                ids.push_back(pool[static_cast<size_t>(last)]);
            }
        }
        return true;
    };
    struct Layout
    {
        std::array<int, RackCount> firstSlot{};
        int slots = 0;
    };
    auto planFor = [&](const std::array<size_t, RackCount>& outerIdx, Layout& lay) {
        SearchSpec spec;
        spec.base = base_;
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            if (inner >> r & 1)
                continue;
            const Sets& s = r == split ? prefix[ri] : full[ri];
            for (int j = 0; j < s.width; ++j)
                if (int c = s.at(outerIdx[ri], j); c >= 0)
                    spec.base.modules.push_back(cands_[static_cast<size_t>(c)].input);
        }
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            lay.firstSlot[ri] = static_cast<int>(spec.slots.size());
            const int n = (inner >> r & 1) ? slots_[ri] : r == split ? 1 : 0;
            if (!n)
                continue;
            std::vector<ModuleInput> in;
            for (int c : lists[ri])
                in.push_back(cands_[static_cast<size_t>(c)].input);
            for (int j = 0; j < n; ++j)
                spec.slots.push_back(in);
        }
        lay.slots = static_cast<int>(spec.slots.size());
        return std::make_unique<SearchPlan>(engine_, spec);
    };
    struct Best
    {
        Scored s;
        uint64_t index = std::numeric_limits<uint64_t>::max();
        std::vector<int> ids;
    };
    const size_t W = static_cast<size_t>(pool_->size());
    std::vector<Best> bestW(W);
    std::vector<size_t> fitW(W, 0), evalW(W, 0);
    auto evalOne = [&](const SearchPlan& plan, const Layout& lay, const std::array<size_t, RackCount>& outerIdx,
                       uint64_t o, uint64_t i, int w, std::vector<int>& ids, std::vector<int>& choice) {
        std::array<size_t, RackCount> idx = outerIdx;
        int last = 0;
        decodeInner(i, idx, last);
        if (!idsOf(idx, last, ids) || !validIds(ids))
            return;
        choice.assign(static_cast<size_t>(std::max(1, lay.slots)), -1);
        double price = 0;
        for (int c : ids)
            price += cands_[static_cast<size_t>(c)].price;
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            if (inner >> r & 1)
                for (int j = 0; j < slots_[ri]; ++j)
                    if (int c = full[ri].at(idx[ri], j); c >= 0)
                        choice[static_cast<size_t>(lay.firstSlot[ri] + j)] = optIndex[static_cast<size_t>(c)];
            if (r == split && last < static_cast<int>(lists[ri].size()))
                choice[static_cast<size_t>(lay.firstSlot[ri])] = last;
        }
        ++evalW[static_cast<size_t>(w)];
        TestResult t = plan.evaluate(choice.data(), goal_);
        if (!(t.goal > 0) || !fitsOf(t))
            return;
        ++fitW[static_cast<size_t>(w)];
        Scored s;
        s.valid = s.fits = true;
        s.goal = t.goal;
        s.bucket = goalBucket(t.goal);
        s.price = price;
        s.r = t;
        Best& b = bestW[static_cast<size_t>(w)];
        const uint64_t index = o * nInner + i;
        if (better(s, b.s) || (!better(b.s, s) && index < b.index))
            b = {s, index, ids};
    };
    if (nOuter >= 4 * W)
        pool_->run(static_cast<size_t>(nOuter), 1, [&](size_t o, int w) {
            std::array<size_t, RackCount> outerIdx{};
            decodeOuter(o, outerIdx);
            Layout lay;
            auto plan = planFor(outerIdx, lay);
            std::vector<int> ids, choice;
            for (uint64_t i = 0; i < nInner; ++i)
                evalOne(*plan, lay, outerIdx, o, i, w, ids, choice);
        });
    else
        for (uint64_t o = 0; o < nOuter; ++o)
        {
            std::array<size_t, RackCount> outerIdx{};
            decodeOuter(o, outerIdx);
            Layout lay;
            auto plan = planFor(outerIdx, lay);
            pool_->run(static_cast<size_t>(nInner), 2048, [&](size_t i, int w) {
                thread_local std::vector<int> ids, choice;
                evalOne(*plan, lay, outerIdx, o, i, w, ids, choice);
            });
        }
    Best b;
    size_t evaluated = 0;
    for (size_t w = 0; w < W; ++w)
    {
        mc.fitting += fitW[w];
        evaluated += evalW[w];
        if (better(bestW[w].s, b.s) || (!better(b.s, bestW[w].s) && bestW[w].index < b.index))
            b = bestW[w];
    }
    mc.ran = true;
    mc.combinations = static_cast<double>(evaluated);
    mc.plans = static_cast<double>(nOuter);
    mc.seconds = secondsSince(t0);
    if (b.s.fits)
    {
        mc.best = b.s;
        mc.ids = b.ids;
    }
    return mc;
}

void Search::memberBrute()
{
    MemberCheck& mc = result_.members;
    if (allChoices_[RackMid].empty() && allChoices_[RackLow].empty() && allChoices_[RackRig].empty())
        buildPool();
    const Enumerated e = enumerate(allChoices_, 5e9);
    mc.skipped = e.skipped;
    if (!e.ran)
        return;
    mc.ran = true;
    mc.combinations = e.combinations;
    mc.fitting = e.fitting;
    mc.seconds = e.seconds;
    if (e.best.fits)
    {
        mc.best = e.best.goal;
        mc.bestPrice = e.best.price;
        std::map<std::string, int> names;
        for (int c : e.ids)
            ++names[std::string(rackName(cands_[static_cast<size_t>(c)].rack)) + " " + candName(c)];
        for (const auto& [n, k] : names)
            mc.bestModules += (mc.bestModules.empty() ? "" : "; ") + (k > 1 ? std::to_string(k) + "x " : std::string()) + n;
    }
    logLine("member brute force: " + std::to_string(e.combinations) + " combinations (" + std::to_string(e.plans) +
            " plans), " + std::to_string(mc.fitting) + " fit; best " + std::to_string(mc.best) + " [" + mc.bestModules +
            "] (" + std::to_string(mc.seconds) + " s)");
}

// Every combination of the stage-1 kinds, evaluated exactly: the core and resist
// kinds of each rack (multisets within the slot counts and limits, empty slots
// included), each with the first fitting-module fill that makes it fit. Their
// scores, best first, must equal stage 1's top sets: this checks the bounds and
// prefilters (a missed set shows as a lower score at some rank) and the model.
void Search::bruteForce()
{
    BruteForceCheck& chk = result_.check;
    chk.ran = true;
    const std::unordered_map<int, int> groupCap = groupCaps();
    std::array<std::vector<int>, RackCount> valueKinds, options;
    std::array<std::vector<Multiset>, RackCount> valueMs, fillMs;
    auto multisets = [&](const std::vector<int>& list, int r) {
        std::vector<int> caps, groups;
        for (int k : list)
        {
            caps.push_back(kinds_[static_cast<size_t>(k)].cap);
            groups.push_back(kinds_[static_cast<size_t>(k)].groupID);
        }
        return rackMultisets(list, slots_[static_cast<size_t>(r)], caps, groups, groupCap);
    };
    SearchSpec spec;
    spec.base = base_;
    std::array<int, RackCount> firstSlot{};
    int items = 0;
    for (int r = 0; r < RackCount; ++r)
    {
        const size_t ri = static_cast<size_t>(r);
        valueKinds[ri] = coreKinds_[ri];
        valueKinds[ri].insert(valueKinds[ri].end(), resistKinds_[ri].begin(), resistKinds_[ri].end());
        valueMs[ri] = multisets(valueKinds[ri], r);
        fillMs[ri] = multisets(fitKinds_[ri], r);
        options[ri] = valueKinds[ri];
        options[ri].insert(options[ri].end(), fitKinds_[ri].begin(), fitKinds_[ri].end());
        firstSlot[ri] = static_cast<int>(spec.slots.size());
        std::vector<ModuleInput> in;
        for (int k : options[ri])
        {
            in.push_back(cands_[static_cast<size_t>(kinds_[static_cast<size_t>(k)].cand)].input);
            items += (in.back().chargeID ? 2 : 1) * slots_[ri];
        }
        for (int s = 0; s < slots_[ri]; ++s)
            spec.slots.push_back(in);
    }
    if (items > kPlanItemBudget - 2 * static_cast<int>(base_.modules.size()) - 4)
        throw std::runtime_error("brute force: too many open slots for one plan (lower Options::maxSlots)");
    SearchPlan plan(engine_, spec);
    const int slotCount = static_cast<int>(spec.slots.size());

    // Group limits as indices; fill combinations (product over racks) by total size.
    std::vector<int> capOf(kinds_.size(), -1), capLimit;
    {
        std::unordered_map<int, int> index;
        for (size_t k = 0; k < kinds_.size(); ++k)
            if (auto g = groupCap.find(kinds_[k].groupID); g != groupCap.end())
            {
                auto [it, inserted] = index.emplace(g->first, static_cast<int>(capLimit.size()));
                if (inserted)
                    capLimit.push_back(g->second);
                capOf[k] = it->second;
            }
    }
    std::vector<std::array<uint32_t, RackCount>> fills;
    {
        std::array<uint32_t, RackCount> cur{};
        std::function<void(int)> rec = [&](int r) {
            if (r == RackCount)
            {
                fills.push_back(cur);
                return;
            }
            for (size_t i = 0; i < fillMs[static_cast<size_t>(r)].size(); ++i)
            {
                cur[static_cast<size_t>(r)] = static_cast<uint32_t>(i);
                rec(r + 1);
            }
        };
        rec(0);
        auto size = [&](const std::array<uint32_t, RackCount>& f) {
            int n = 0;
            for (int r = 0; r < RackCount; ++r)
                n += fillMs[static_cast<size_t>(r)][f[static_cast<size_t>(r)]].n;
            return n;
        };
        std::stable_sort(fills.begin(), fills.end(), [&](const auto& a, const auto& b) { return size(a) < size(b); });
    }

    double total = 1;
    for (int r = 0; r < RackCount; ++r)
        total *= static_cast<double>(valueMs[static_cast<size_t>(r)].size());
    if (total > 2e9)
        throw std::runtime_error("brute force: too many combinations (lower Options::maxSlots)");
    chk.combinations = total;
    struct Hit
    {
        double goal;
        uint64_t index;
    };
    std::vector<std::vector<Hit>> hits(static_cast<size_t>(pool_->size()));
    std::vector<size_t> goalChanges(static_cast<size_t>(pool_->size()), 0);
    auto decode = [&](uint64_t i) {
        std::array<uint32_t, RackCount> idx{};
        for (int r = RackCount - 1; r >= 0; --r)
        {
            uint64_t n = valueMs[static_cast<size_t>(r)].size();
            idx[static_cast<size_t>(r)] = static_cast<uint32_t>(i % n);
            i /= n;
        }
        return idx;
    };
    pool_->run(static_cast<size_t>(total), 256, [&](size_t i, int worker) {
        const auto idx = decode(i);
        std::vector<int> choice(static_cast<size_t>(slotCount), -1);
        std::vector<int> groupUsed(capLimit.size(), 0);
        std::array<int, RackCount> used{};
        for (int r = 0; r < RackCount; ++r)
        {
            const Multiset& m = valueMs[static_cast<size_t>(r)][idx[static_cast<size_t>(r)]];
            for (int j = 0; j < m.n; ++j)
            {
                int k = valueKinds[static_cast<size_t>(r)][m.k[static_cast<size_t>(j)]];
                if (capOf[static_cast<size_t>(k)] >= 0)
                    ++groupUsed[static_cast<size_t>(capOf[static_cast<size_t>(k)])];
                choice[static_cast<size_t>(firstSlot[static_cast<size_t>(r)] + used[static_cast<size_t>(r)]++)] =
                    m.k[static_cast<size_t>(j)];
            }
        }
        for (size_t g = 0; g < capLimit.size(); ++g)
            if (groupUsed[g] > capLimit[g])
                return;
        TestResult t = plan.evaluate(choice.data(), goal_);
        if (t.goal <= 0)
            return;
        bool fits = fitsOf(t);
        for (size_t f = 0; f < fills.size() && !fits; ++f)
        {
            std::vector<int> ch = choice;
            std::vector<int> gu = groupUsed;
            bool ok = true;
            for (int r = 0; r < RackCount && ok; ++r)
            {
                const size_t ri = static_cast<size_t>(r);
                const Multiset& m = fillMs[ri][fills[f][ri]];
                ok = used[ri] + m.n <= slots_[ri];
                for (int j = 0; j < m.n && ok; ++j)
                {
                    int local = m.k[static_cast<size_t>(j)];
                    int k = fitKinds_[ri][static_cast<size_t>(local)];
                    if (capOf[static_cast<size_t>(k)] >= 0 &&
                        ++gu[static_cast<size_t>(capOf[static_cast<size_t>(k)])] >
                            capLimit[static_cast<size_t>(capOf[static_cast<size_t>(k)])])
                        ok = false;
                    ch[static_cast<size_t>(firstSlot[ri] + used[ri] + j)] =
                        static_cast<int>(valueKinds[ri].size()) + local;
                }
            }
            if (!ok)
                continue;
            TestResult tf = plan.evaluate(ch.data(), goal_);
            if (std::abs(tf.goal - t.goal) > 1e-6 * std::abs(t.goal))
                ++goalChanges[static_cast<size_t>(worker)];
            fits = fitsOf(tf);
        }
        if (fits)
            hits[static_cast<size_t>(worker)].push_back({t.goal, i});
    });
    std::vector<Hit> all;
    for (size_t w = 0; w < hits.size(); ++w)
    {
        all.insert(all.end(), hits[w].begin(), hits[w].end());
        chk.fittingGoalChanges += goalChanges[w];
    }
    std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) {
        return a.goal != b.goal ? a.goal > b.goal : a.index < b.index;
    });
    chk.feasible = all.size();

    auto names = [&](const std::vector<int>& ks) {
        std::map<std::string, int> n;
        for (int k : ks)
            ++n[std::string(rackName(kinds_[static_cast<size_t>(k)].rack)) + " " +
                candName(kinds_[static_cast<size_t>(k)].cand)];
        std::string s;
        for (const auto& [name, c] : n)
            s += (s.empty() ? "" : "; ") + (c > 1 ? std::to_string(c) + "x " : std::string()) + name;
        return s.empty() ? std::string("(empty)") : s;
    };
    auto bruteKinds = [&](uint64_t i) {
        std::vector<int> ks;
        const auto idx = decode(i);
        for (int r = 0; r < RackCount; ++r)
        {
            const Multiset& m = valueMs[static_cast<size_t>(r)][idx[static_cast<size_t>(r)]];
            for (int j = 0; j < m.n; ++j)
                ks.push_back(valueKinds[static_cast<size_t>(r)][m.k[static_cast<size_t>(j)]]);
        }
        return ks;
    };
    auto stage1Kinds = [&](const Found& f) {
        std::vector<int> ks;
        for (int r = 0; r < RackCount; ++r)
        {
            const size_t ri = static_cast<size_t>(r);
            const Multiset& m = coreMs_[ri][f.ms[ri]];
            for (int j = 0; j < m.n; ++j)
                ks.push_back(coreKinds_[ri][m.k[static_cast<size_t>(j)]]);
            const Multiset& rm = resistMs_[ri][resistSets_[static_cast<size_t>(f.rset)].ms[ri]];
            for (int j = 0; j < rm.n; ++j)
                ks.push_back(resistKinds_[ri][rm.k[static_cast<size_t>(j)]]);
        }
        return ks;
    };

    const size_t K = static_cast<size_t>(std::max(1, opt_.topK));
    const size_t expect = std::min(K, all.size());
    chk.compared = std::min(expect, top_.size());
    if (top_.size() != expect)
    {
        chk.mismatches += top_.size() > expect ? top_.size() - expect : expect - top_.size();
        logLine("brute force: stage 1 kept " + std::to_string(top_.size()) + " sets, expected " + std::to_string(expect));
    }
    if (!all.empty())
        chk.bruteBest = all.front().goal;
    if (!top_.empty())
        chk.stage1Best = top_.front().score;
    int shown = 0;
    for (size_t i = 0; i < chk.compared; ++i)
    {
        double a = all[i].goal, b = top_[i].score;
        double err = std::abs(a - b) / std::max(std::abs(a), 1e-9);
        chk.maxError = std::max(chk.maxError, err);
        if (err <= 1e-5)
            continue;
        ++chk.mismatches;
        if (shown++ < 5 && log_)
            *log_ << "  brute force rank " << i + 1 << ": exact " << a << " [" << names(bruteKinds(all[i].index))
                  << "] vs stage 1 " << b << " [" << names(stage1Kinds(top_[i])) << "]\n";
    }
    logLine("brute force: " + std::to_string(static_cast<uint64_t>(total)) + " combinations, " +
            std::to_string(all.size()) + " fit; best " + std::to_string(chk.bruteBest) + " vs stage 1 " +
            std::to_string(chk.stage1Best) + "; " + std::to_string(chk.compared) + " ranks compared, " +
            std::to_string(chk.mismatches) + " mismatches, max error " + std::to_string(chk.maxError) +
            (chk.fittingGoalChanges ? "; fitting modules changed the goal " + std::to_string(chk.fittingGoalChanges) + "x"
                                    : std::string()));
    if (!all.empty())
        logLine("  best: " + names(bruteKinds(all.front().index)));
}

// ---- infeasibility
// Only the pinned part can make every option unfittable: an open slot may stay
// empty, so when the pinned part alone fits, the empty choice fits. When it is over
// on some resource, three checks follow, each before the more expensive work:
// calibration (before candidates), the output and cost bounds (after measuring the
// candidates), and the search itself.

bool Search::pinnedOver() const
{
    for (size_t k = 0; k < 3; ++k)
        if (pinnedUsed_[k] > pinnedOutput_[k])
            return true;
    return false;
}

std::string Search::fmt(double v) const
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string Search::selectionNote() const
{
    std::string n;
    if (opt_.priceCap > 0)
        n += " (with the price cap of " + fmt(opt_.priceCap / 1e6) + "M ISK per module)";
    return n;
}

// Calibration: no module raises calibration output and nothing lowers a rig's
// calibration cost, and open rig slots only add rigs. So if the pinned rigs alone
// are over, nothing in the open slots can help: the user has to change a pinned rig.
bool Search::diagnoseCalibration()
{
    const double used = pinnedUsed_[ResCalibration], out = pinnedOutput_[ResCalibration];
    if (!(used > out))
        return false;
    Infeasibility& inf = result_.infeasible;
    inf = {};
    inf.infeasible = true;
    inf.reason = "calibration";
    const double excess = used - out;
    // Each pinned rig's calibration: the fit's use without it.
    std::vector<std::pair<int, double>> rigs;  // (base_ index, calibration)
    for (size_t j = 0; j < base_.modules.size(); ++j)
    {
        auto rack = rackOf(gd_, base_.modules[j].typeID);
        if (!rack || *rack != RackRig)
            continue;
        FitInput without = base_;
        without.modules.erase(without.modules.begin() + static_cast<ptrdiff_t>(j));
        const double cal = used - static_cast<double>(toTestResult(engine_.evaluate(without), goal_).calibrationUsed);
        if (cal > 0)
            rigs.emplace_back(static_cast<int>(j), cal);
    }
    // The rigs whose removal alone fixes it; if none does, all of them.
    std::vector<std::pair<int, double>> named;
    for (const auto& r : rigs)
        if (r.second >= excess)
            named.push_back(r);
    const bool single = !named.empty();
    if (!single)
        named = rigs;
    // Listed once per (type, cost): "2x Name (150.0 each)".
    std::vector<std::pair<std::pair<int, double>, int>> groups;  // ((typeID, cost), count)
    for (const auto& [j, cal] : named)
    {
        inf.modules.push_back(pinnedIndex_[static_cast<size_t>(j)]);
        inf.moduleCost.push_back(cal);
        const int type = base_.modules[static_cast<size_t>(j)].typeID;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
            return g.first.first == type && std::abs(g.first.second - cal) < 1e-6;
        });
        if (it == groups.end())
            groups.push_back({{type, cal}, 1});
        else
            ++it->second;
    }
    std::string list;
    for (const auto& [key, n] : groups)
    {
        const Type* t = gd_.type(key.first);
        list += std::string(list.empty() ? "" : ", ") + (n > 1 ? std::to_string(n) + "x " : std::string()) +
                (t ? t->name : std::string("rig")) + " (" + fmt(key.second) + (n > 1 ? " each)" : ")");
    }
    Shortfall sh;
    sh.resource = ResCalibration;
    sh.pinnedUsed = used;
    sh.output = out;
    sh.bestPinnedUsed = used;
    sh.bestOutput = out;
    sh.shortfall = excess;
    sh.message = "The rigs outside the selection use " + fmt(used) + " calibration of " + fmt(out) + " (" + fmt(excess) +
                 " over). No module raises calibration or lowers a rig's calibration cost, so no choice for the "
                 "selected slots can fix this. ";
    if (list.empty())
        sh.message += "Change the pinned modules.";
    else if (single)
        sh.message += "To fix it, select one of these rigs (to replace it) or remove it: " + list + ".";
    else
        sh.message += "To fix it, select or remove rigs worth at least " + fmt(excess) + " calibration: " + list + ".";
    inf.message = sh.message;
    inf.resources.push_back(std::move(sh));
    logLine("infeasible (calibration): " + inf.message);
    return true;
}

// CPU / power: necessary conditions, after measuring every candidate alone on the
// pinned fit. Per open slot, the strongest allowed output gain (compounded as a
// multiplier: stacking penalties only lower it) and, separately, the largest drop of
// the pinned modules' needs (added up: several reductions of one module compound to
// less than their sum). Open modules' own costs are >= 0. If even these bounds leave
// the pinned modules over, no set fits. Calibration: no output gain, no reduction.
bool Search::diagnoseOutputs()
{
    if (result_.infeasible.infeasible)
        return true;  // calibration (bruteForce runs on)
    if (!pinnedOver())
        return false;
    std::vector<Shortfall> list;
    bool impossible = false;
    static const char* label[3] = {"CPU", "powergrid", "calibration"};
    static const char* unit[3] = {" tf", " MW", ""};
    static const char* help[3] = {"co-processors or CPU rigs", "power diagnostic systems, reactor control units or "
                                                               "power rigs", ""};
    for (size_t k = 0; k < 3; ++k)
    {
        if (!(pinnedUsed_[k] > pinnedOutput_[k]))
            continue;
        Shortfall sh;
        sh.resource = static_cast<int>(k);
        sh.pinnedUsed = pinnedUsed_[k];
        sh.output = pinnedOutput_[k];
        double out = pinnedOutput_[k], used = pinnedUsed_[k];
        bool raises = false;
        if (k != ResCalibration)
            for (int r = 0; r < RackCount; ++r)
            {
                std::vector<double> gain, drop;
                for (const Candidate& c : cands_)
                    if (c.rack == r)
                    {
                        gain.push_back(std::max(0.0, c.outputDelta[k]));
                        drop.push_back(c.pinnedDrop[k]);
                    }
                std::sort(gain.begin(), gain.end(), std::greater<>());
                std::sort(drop.begin(), drop.end(), std::greater<>());
                const size_t n = static_cast<size_t>(slots_[static_cast<size_t>(r)]);
                for (size_t i = 0; i < n && i < gain.size(); ++i)
                {
                    if (gain[i] > 0 && pinnedOutput_[k] > 0)
                        out *= 1 + gain[i] / pinnedOutput_[k], raises = true;
                    used -= drop[i];
                    raises |= drop[i] > 0;
                }
            }
        used = std::max(0.0, used);
        sh.bestPinnedUsed = used;
        sh.bestOutput = out;
        sh.shortfall = std::max(0.0, used - out);
        // Pad against float noise: only a clear shortfall counts.
        const bool over = used > out * (1 + 1e-6) + 0.01;
        impossible |= over;
        const std::string head = "The modules outside the selection need " + fmt(sh.pinnedUsed) + unit[k] + " " +
                                 label[k] + " of " + fmt(sh.output) + unit[k] + ".";
        if (!over)
            sh.message = head + " Modules in the selected slots may make up for it.";
        else if (!raises)
            sh.message = head + " No module allowed in the selected slots" + selectionNote() + " raises " + label[k] +
                         " or lowers the other modules' " + label[k] + " needs. Select slots for " + help[k] +
                         ", or remove a module outside the selection.";
        else
            sh.message = head + " With the selected slots" + selectionNote() + ", " + label[k] + " can reach at most " +
                         fmt(out) + unit[k] + " and the other modules need at least " + fmt(used) + unit[k] + " (" +
                         fmt(used - out) + " short). Select more slots for " + help[k] +
                         ", or remove a module outside the selection.";
        list.push_back(std::move(sh));
    }
    bounds_ = list;
    if (!impossible)
        return false;
    Infeasibility& inf = result_.infeasible;
    inf = {};
    inf.infeasible = true;
    inf.reason = "outputs";
    for (const Shortfall& sh : list)
        if (sh.shortfall > 0)
            inf.message += (inf.message.empty() ? "" : " ") + sh.message;
    inf.resources = std::move(list);
    logLine("infeasible (outputs): " + inf.message);
    return true;
}

// After the search: no set fits (found = false). If the pinned part is over and no
// fitting-module fill alone makes it fit, report it. (If the pinned part fits, or a
// fill does, a fitting choice exists; then nothing reached a goal above 0.)
void Search::diagnoseSearch(bool found)
{
    if (result_.infeasible.infeasible)
    {
        result_.fits.clear();
        return;
    }
    if (found)
        return;
    if (!pinnedOver())
    {
        noGoalWarning();
        return;
    }
    if (!fitSets_.empty())
    {
        // The pinned modules' costs (as stage 1 counts them) plus a fitting-module fill.
        std::vector<uint8_t> none(kinds_.size(), 0);
        CoreEval ce;
        assemble(none.data(), ce);
        if (findFitting(none.data(), slots_, ce.cpu, ce.power, ce.calibration) >= 0)
        {
            noGoalWarning();
            return;
        }
    }
    Infeasibility& inf = result_.infeasible;
    inf = {};
    inf.infeasible = true;
    inf.reason = "search";
    static const char* label[3] = {"CPU", "powergrid", "calibration"};
    static const char* unit[3] = {" tf", " MW", ""};
    std::string head;
    for (size_t k = 0; k < 3; ++k)
    {
        if (!(pinnedUsed_[k] > pinnedOutput_[k]))
            continue;
        Shortfall sh;
        sh.resource = static_cast<int>(k);
        sh.pinnedUsed = pinnedUsed_[k];
        sh.output = pinnedOutput_[k];
        sh.bestPinnedUsed = sh.pinnedUsed;
        sh.bestOutput = sh.output;
        for (const Shortfall& b : bounds_)
            if (b.resource == sh.resource)
                sh.bestPinnedUsed = b.bestPinnedUsed, sh.bestOutput = b.bestOutput;
        sh.shortfall = 0;  // the bounds allow it: the search decided
        sh.message = "The modules outside the selection need " + fmt(sh.pinnedUsed) + unit[k] + " " + label[k] + " of " +
                     fmt(sh.output) + unit[k] + ".";
        if (sh.bestOutput > sh.output || sh.bestPinnedUsed < sh.pinnedUsed)
            sh.message += " The selected slots can raise it to at most " + fmt(sh.bestOutput) + unit[k] +
                          " and lower the need to at least " + fmt(sh.bestPinnedUsed) + unit[k] +
                          " (bounds), but no combination fits once the modules' own costs are counted.";
        head += (head.empty() ? "" : " ") + sh.message;
        inf.resources.push_back(std::move(sh));
    }
    inf.message = head + " No combination of modules allowed in the selected slots" + selectionNote() +
                  " makes the fit fit. Select more slots for fitting modules, or remove a module outside the selection.";
    logLine("infeasible (search): " + inf.message);
}

// Some choice fits, but none reaches a goal above 0 (e.g. nothing pinned or allowed
// in the selected slots repairs the goal's layer, or what would doesn't fit).
void Search::noGoalWarning()
{
    std::string w = "no choice for the selected slots that fits gives a goal above 0";
    w += " (free with the selected slots empty: " + fmt(pinnedOutput_[ResCpu] - pinnedUsed_[ResCpu]) + " tf CPU, " +
         fmt(pinnedOutput_[ResPower] - pinnedUsed_[ResPower]) + " MW powergrid, " +
         fmt(pinnedOutput_[ResCalibration] - pinnedUsed_[ResCalibration]) + " calibration)";
    result_.warnings.push_back(w);
}

Result Search::run()
{
    auto t0 = Clock::now();
    {
        int threads = opt_.threads > 0 ? opt_.threads : static_cast<int>(std::thread::hardware_concurrency());
        pool_ = std::make_unique<Pool>(std::max(1, threads));
    }
    auto progress = [&](double f, const char* phase) {
        if (opt_.progress)
            opt_.progress(f, phase);
    };
    auto finish = [&] {
        progress(1, "done");
        result_.stats.seconds[4] = secondsSince(t0);
        result_.stats.engineCalls = calls_.load();
        return std::move(result_);
    };
    progress(0, "setup");
    setup();
    // Fast exits when no option can fit. With bruteForce the search still runs, so
    // the validation can confirm the diagnosis (no feasible combination).
    if (diagnoseCalibration() && !opt_.bruteForce)
        return finish();
    if (slots_[RackMid] + slots_[RackLow] + slots_[RackRig] == 0)
    {
        result_.warnings.push_back("no open slots");
        diagnoseOutputs();  // no candidates: the pinned part as it is
        return finish();
    }
    progress(0.02, "candidates");
    gatherCandidates();
    progress(0.05, "measuring candidates");
    measure();
    if (diagnoseOutputs() && !opt_.bruteForce)
        return finish();
    progress(0.10, "interactions");
    interactions();
    classify();
    // Stage 1's model has size limits (ModelLimit). When one is hit, stages 1 and 2 are
    // skipped and refine searches alone (from the empty set); it needs no model.
    bool model = true;
    try
    {
        buildKinds();
    }
    catch (const ModelLimit& e)
    {
        if (opt_.bruteForce || !opt_.refine || opt_.dumpKinds)
            throw;
        model = false;
        result_.stats.modelSkipped = e.what();
    }
    logLine(std::to_string(result_.stats.candidates) + " useful candidates in " + std::to_string(kinds_.size()) +
            " subgroups (" + std::to_string(result_.stats.coreKinds) + " core, " +
            std::to_string(result_.stats.resistKinds) + " resist, " + std::to_string(result_.stats.fittingKinds) +
            " fitting)");
    result_.stats.seconds[0] = secondsSince(t0);
    if (opt_.dumpKinds)
        return std::move(result_);  // diagnostics only
    try
    {
        if (!model)
            throw ModelLimit(result_.stats.modelSkipped);
        auto t1 = Clock::now();
        progress(0.15, "model");
        buildCorePlan();
        buildResistSets();
        result_.stats.seconds[1] = secondsSince(t1);
        auto t2 = Clock::now();
        progress(0.25, "stage 1");
        stage1();
        result_.stats.seconds[2] = secondsSince(t2);
        if (opt_.bruteForce)
        {
            progress(0.7, "brute force");
            bruteForce();
            diagnoseSearch(!top_.empty());
            return finish();
        }
        auto t3 = Clock::now();
        progress(0.7, "stage 2");
        stage2();
        result_.stats.seconds[3] = secondsSince(t3);
    }
    catch (const ModelLimit& e)
    {
        if (opt_.bruteForce || !opt_.refine)
            throw;
        result_.stats.modelSkipped = e.what();
        logLine(std::string("stage 1 model limit (") + e.what() + "): stages 1 and 2 skipped, refine only");
        top_.clear();
        finals_.clear();
    }
    if (opt_.refine)
    {
        auto t4 = Clock::now();
        progress(0.8, "refine");
        refine(t0);
        result_.stats.seconds[5] = secondsSince(t4);
    }
    else
        result_.stats.refineStop = "off";
    report();
    diagnoseSearch(!result_.fits.empty());
    if (opt_.memberBruteForce)
    {
        result_.stats.seconds[4] = secondsSince(t0);
        progress(0.95, "member brute force");
        memberBrute();
        progress(1, "done");
        result_.stats.engineCalls = calls_.load();
        return std::move(result_);
    }
    return finish();
}
}  // namespace

Result optimize(const GameData& gd, const Engine& engine, const FitInput& fit, const Options& options)
{
    Search s(gd, engine, fit, options);
    return s.run();
}
}  // namespace fitengine::opt
