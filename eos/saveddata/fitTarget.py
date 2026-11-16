# ===============================================================================
# Copyright (C) 2010 Diego Duclos
#
# This file is part of eos.
#
# eos is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# eos is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with eos.  If not, see <http://www.gnu.org/licenses/>.
# ===============================================================================

# A fit's target (the Target tab of the additions pane): another fit or a target
# profile the fit is aimed at. A fit keeps a list of them, at most one of which is
# active. Targets don't take part in the fit's calculation. Mapped in
# eos.db.saveddata.fit (table fitTargets).

from eos.saveddata.targetProfile import TargetProfile


KIND_FIT = 'fit'
KIND_PROFILE = 'profile'


class FitTarget:

    def __init__(self, target, active=False):
        self.target = target
        self.active = active

    @property
    def targetFit(self):
        return self.__targetFit

    @property
    def targetProfile(self):
        if self.__userTargetProfile is not None:
            return self.__userTargetProfile
        if self.__builtinTargetProfileID is not None:
            # The ideal target is a builtin with ID 0 which is not in the builtin list
            if self.__builtinTargetProfileID == TargetProfile.getIdeal().ID:
                return TargetProfile.getIdeal()
            return TargetProfile.getBuiltinById(self.__builtinTargetProfileID)
        return None

    @property
    def target(self):
        if self.__targetFit is not None:
            return self.__targetFit
        return self.targetProfile

    @target.setter
    def target(self, target):
        self.__targetFit = None
        self.__userTargetProfile = None
        self.__builtinTargetProfileID = None
        if isinstance(target, TargetProfile):
            if target.builtin:
                self.__builtinTargetProfileID = target.ID
            else:
                self.__userTargetProfile = target
        elif target is not None:
            self.__targetFit = target

    @property
    def kind(self):
        if self.__targetFit is not None:
            return KIND_FIT
        if self.targetProfile is not None:
            return KIND_PROFILE
        return None

    @property
    def isFit(self):
        return self.kind == KIND_FIT

    @property
    def isProfile(self):
        return self.kind == KIND_PROFILE

    @property
    def isInvalid(self):
        target = self.target
        if target is None:
            return True
        return self.isFit and target.isInvalid

    @property
    def key(self):
        """
        Identifies the target within a fit's target list (a fit holds each target once):
        ('fit', fitID) or ('profile', profileID). Builtin profile IDs are negative, the
        ideal target's is 0 and user profiles' are positive, so profile IDs don't clash.
        """
        target = self.target
        if target is None:
            return None
        return self.kind, target.ID

    def __repr__(self):
        return "FitTarget(key={}, active={}) at {}".format(self.key, self.active, hex(id(self)))
