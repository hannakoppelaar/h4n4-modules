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
#include "utils.hpp"

bool exists(const char *fileName) {
    struct stat info;
    if (stat(fileName, &info) != 0) {
        return false;
    } else if (info.st_mode & S_IFDIR) {
        return true;
    } else {
        return false;
    }
}

// Naive attempt to get the parent directory (we're stuck with C++11 for now)
std::string getParentDir(const char *fileName) {
    std::string fn = fileName;
    std::string candidate = fn.substr(0, fn.find_last_of("/\\"));
    if (exists(candidate.c_str())) {
        return candidate;
    } else {
        return NULL;
    }
}

std::string getBaseName(const char *fileName) {
    std::string fileNameStr = fileName;
    return fileNameStr.substr(fileNameStr.find_last_of("/\\") + 1);
}
