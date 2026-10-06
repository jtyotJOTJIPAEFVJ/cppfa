#pragma once

// EFT text export, matching pyfa's service/port/eft.py exportEft() with all
// options enabled (implants, boosters, cargo, loaded charges, mutations).

#include <string>
#include <vector>

#include "Fit.h"

class GameData;

std::string exportEft(const Fit& fit, const GameData& gd, std::vector<std::string>* warnings = nullptr);
