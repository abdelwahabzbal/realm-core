////////////////////////////////////////////////////////////////////////////
//
// Copyright 2019 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#if TEST_SCHEDULER_UV
#include <realm/object-store/util/uv/scheduler.hpp>
#endif

#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include <catch2/catch_all.hpp>

#include <limits.h>

#ifdef _MSC_VER
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#else
#include <libgen.h>
#include <unistd.h>
#endif

int main(int argc, char** argv)
{
#ifdef _MSC_VER
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, sizeof(path)) == 0) {
        fprintf(stderr, "Failed to retrieve path to executable.\n");
        return 1;
    }
    PathRemoveFileSpecA(path);
    SetCurrentDirectoryA(path);
#else
    char executable[PATH_MAX];
    (void)realpath(argv[0], executable);
    const char* directory = dirname(executable);
    chdir(directory);
#endif

#if TEST_SCHEDULER_UV
    realm::util::Scheduler::set_default_factory([]() -> std::shared_ptr<realm::util::Scheduler> {
        return std::make_shared<realm::util::UvMainLoopScheduler>();
    });
#endif

    int result = Catch::Session().run(argc, argv);
    return result < 0xff ? result : 0xff;
}
