#include "nix/store/builtins.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/util/strings.hh"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace nix {

/**
 * Builtin builder: cproject-pkgconfig
 *
 * Queries pkg-config for compiler and linker flags for specified dependencies.
 * Outputs JSON with the flags.
 *
 * Environment variables:
 * - pkgConfigDeps: Space-separated list of package names to query
 *
 * Output (JSON):
 * {
 *   "cflags": ["-I/nix/store/.../include", "-DFOO=1", ...],
 *   "libs": ["-L/nix/store/.../lib", "-lssl", "-lcrypto", ...]
 * }
 */
static void builtinCProjectPkgConfig(const BuiltinBuilderContext & ctx)
{
    auto out = get(ctx.drv.outputs, "out");
    if (!out)
        throw Error("'builtin:cproject-pkgconfig' requires an 'out' output");

    auto outputPath = ctx.outputs.at("out");

    // Extract configuration from derivation environment
    auto pkgConfigDepsStr = ctx.drv.env.at("pkgConfigDeps");

    // Parse space-separated package names
    std::vector<std::string> packages;
    std::istringstream stream(pkgConfigDepsStr);
    std::string pkg;
    while (stream >> pkg) {
        if (!pkg.empty())
            packages.push_back(pkg);
    }

    json result = json::object();
    json cflagsArray = json::array();
    json libsArray = json::array();

    // Query each package
    for (const auto & package : packages) {
        try {
            // Query cflags
            {
                auto result = runProgram(RunOptions{
                    .program = "pkg-config",
                    .args = {"--cflags", package},
                    .mergeStderrToStdout = false
                });

                // Parse space-separated flags from stdout
                std::istringstream flagStream(result.second);
                std::string flag;
                while (flagStream >> flag) {
                    if (!flag.empty())
                        cflagsArray.push_back(flag);
                }
            }

            // Query libs
            {
                auto result = runProgram(RunOptions{
                    .program = "pkg-config",
                    .args = {"--libs", package},
                    .mergeStderrToStdout = false
                });

                // Parse space-separated flags from stdout
                std::istringstream flagStream(result.second);
                std::string flag;
                while (flagStream >> flag) {
                    if (!flag.empty())
                        libsArray.push_back(flag);
                }
            }

        } catch (Error & e) {
            warn("failed to query pkg-config for %s: %s", package, e.what());
        }
    }

    result["cflags"] = cflagsArray;
    result["libs"] = libsArray;

    // Write result JSON to output
    createDirs(outputPath);
    Path jsonPath = outputPath + "/flags.json";
    std::ofstream outFile(jsonPath);
    outFile << result.dump(2);
    outFile.close();
}

static RegisterBuiltinBuilder registerCProjectPkgConfig("cproject-pkgconfig", builtinCProjectPkgConfig);

} // namespace nix
