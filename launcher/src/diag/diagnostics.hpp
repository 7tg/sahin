// Prospero BlackBox - Durable logs and crash recovery lifecycle.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace store::diag
{
bool start(const std::string &root);
void stop();
// Keeps the log in memory (true) while the disk is busy with an install.
void hold_log(bool hold);
bool recovered();
} // namespace store::diag
