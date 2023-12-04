/**
 * Copyright 2023 Hanna Koppelaar
 *
 * This file is part of the h4n4 collection of VCV modules. This collection is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along with the software. If not,
 * see <https://www.gnu.org/licenses/>.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <string>

bool exists(const char *fileName);

std::string getParentDir(const char *fileName);

std::string getBaseName(const char *fileName);
