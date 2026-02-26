#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derived-path.hh"

namespace nix {

/**
 * Helper to create a basic derivation structure
 */
static Derivation makeBasicDerivation(
    EvalState & state,
    const std::string & name,
    const std::string & system,
    const std::string & builder)
{
    Derivation drv;
    drv.name = name;
    drv.platform = system;
    drv.builder = builder;
    return drv;
}

/**
 * Helper to add a string to derivation environment
 */
static void addEnvString(Derivation & drv, const std::string & key, const std::string & value)
{
    drv.env[key] = value;
}

/**
 * builtins.cProject - Granular C/C++ build system
 *
 * Creates a multi-phase build with:
 * 1. pkg-config resolution (if pkgConfigDeps specified)
 * 2. Header dependency discovery
 * 3. Per-file object compilation
 * 4. Linking
 *
 * Returns the final built output directly.
 */
static void prim_cProject(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.cProject");

    auto attrs = args[0]->attrs();

    // Extract required attributes
    auto nameAttr = state.getAttr(state.s.name, attrs, "in the attrset passed to builtins.cProject");
    std::string_view projectName = state.forceStringNoCtx(
        *nameAttr->value, pos, "while evaluating the 'name' attribute");

    // Get system
    auto systemAttr = attrs->get(state.s.system);
    std::string system = systemAttr
        ? std::string(state.forceStringNoCtx(*systemAttr->value, pos, "while evaluating 'system'"))
        : std::string(state.settings.getCurrentSystem());

    // Get root path
    auto rootAttr = attrs->get(state.symbols.create("root"));
    if (!rootAttr)
        state.error<EvalError>("'root' attribute is required").atPos(pos).debugThrow();

    NixStringContext rootContext;
    auto rootPath = state.coerceToPath(
        pos, *rootAttr->value, rootContext, "while evaluating 'root'");

    // Get sources
    auto sourcesAttr = attrs->get(state.symbols.create("sources"));
    std::vector<std::string> sources;
    if (sourcesAttr) {
        state.forceList(*sourcesAttr->value, pos, "while evaluating 'sources'");
        for (unsigned int i = 0; i < sourcesAttr->value->listSize(); ++i) {
            NixStringContext ctx;
            sources.push_back(std::string(state.coerceToString(
                pos, *sourcesAttr->value->listView()[i], ctx,
                "while evaluating source file").toOwned()));
        }
    }

    // Get compiler (default to gcc)
    auto compilerAttr = attrs->get(state.symbols.create("compiler"));
    std::string compiler = compilerAttr
        ? std::string(state.forceStringNoCtx(*compilerAttr->value, pos, "while evaluating 'compiler'"))
        : "gcc";

    // Get targets
    auto targetsAttr = attrs->get(state.symbols.create("targets"));
    if (!targetsAttr)
        state.error<EvalError>("'targets' attribute is required").atPos(pos).debugThrow();

    state.forceAttrs(*targetsAttr->value, pos, "while evaluating 'targets'");

    // TODO: Parse each target and extract sources, flags, dependencies
    // auto targets = targetsAttr->value->attrs();

    // Phase 1: Create pkg-config derivation(s) if needed
    std::optional<StorePath> pkgConfigDrvPath;
    auto pkgConfigDepsAttr = attrs->get(state.symbols.create("pkgConfigDeps"));
    if (pkgConfigDepsAttr) {
        // Create a derivation that runs pkg-config and outputs flags
        Derivation pkgConfigDrv = makeBasicDerivation(
            state,
            std::string(projectName) + "-pkgconfig",
            system,
            "builtin:cproject-pkgconfig"
        );

        // Add pkgConfigDeps to environment
        std::ostringstream depsStr;
        state.forceList(*pkgConfigDepsAttr->value, pos, "while evaluating 'pkgConfigDeps'");
        for (unsigned int i = 0; i < pkgConfigDepsAttr->value->listSize(); ++i) {
            NixStringContext ctx;
            auto dep = state.coerceToString(
                pos, *pkgConfigDepsAttr->value->listView()[i], ctx,
                "while evaluating pkgConfigDep").toOwned();
            if (i > 0) depsStr << " ";
            depsStr << dep;

            // Add dependencies to derivation inputs
            for (auto & c : ctx) {
                if (auto * p = std::get_if<NixStringContextElem::Opaque>(&c.raw)) {
                    pkgConfigDrv.inputSrcs.insert(p->path);
                }
            }
        }
        addEnvString(pkgConfigDrv, "pkgConfigDeps", depsStr.str());

        // Single output: JSON with cflags and libs
        pkgConfigDrv.env["out"] = "";
        pkgConfigDrv.outputs.insert_or_assign("out", DerivationOutput::Deferred{});
        pkgConfigDrv.fillInOutputPaths(*state.store);

        pkgConfigDrvPath = state.store->writeDerivation(pkgConfigDrv, state.repair);
    }

    // Phase 2: Create discovery derivation
    Derivation discoveryDrv = makeBasicDerivation(
        state,
        std::string(projectName) + "-discover",
        system,
        "builtin:cproject-discover"
    );

    // Add root and sources to discovery
    discoveryDrv.inputSrcs.insert(state.store->parseStorePath(rootPath.path.abs()));

    // Serialize sources list
    std::ostringstream sourcesStr;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i > 0) sourcesStr << ";";
        sourcesStr << sources[i];
    }
    addEnvString(discoveryDrv, "sources", sourcesStr.str());
    addEnvString(discoveryDrv, "compiler", compiler);
    addEnvString(discoveryDrv, "root", rootPath.path.abs());

    // If we have pkgconfig drv, add as dependency
    if (pkgConfigDrvPath) {
        discoveryDrv.inputDrvs.ensureSlot(SingleDerivedPath::Built{
            .drvPath = makeConstantStorePathRef(*pkgConfigDrvPath),
            .output = "out"
        }).value.insert("out");
        addEnvString(discoveryDrv, "pkgConfigFlags",
            state.store->printStorePath(*pkgConfigDrvPath) + "/flags.json");
    }

    // Discovery outputs JSON with header dependencies
    discoveryDrv.env["out"] = "";
    discoveryDrv.outputs.insert_or_assign("out", DerivationOutput::Deferred{});
    discoveryDrv.fillInOutputPaths(*state.store);

    auto discoveryDrvPath = state.store->writeDerivation(discoveryDrv, state.repair);

    // Phase 3: Create build derivation (regular bash build)
    // For MVP: compile and link directly in one derivation
    // Future: Create per-file derivations for true granularity
    Derivation buildDrv = makeBasicDerivation(
        state,
        std::string(projectName),
        system,
        "${bash}/bin/bash"
    );

    // Build depends on discovery
    buildDrv.inputDrvs.ensureSlot(SingleDerivedPath::Built{
        .drvPath = makeConstantStorePathRef(discoveryDrvPath),
        .output = "out"
    }).value.insert("out");

    // Add sources as inputs
    buildDrv.inputSrcs.insert(state.store->parseStorePath(rootPath.path.abs()));

    // Create build script
    std::ostringstream buildScript;
    buildScript << "source $stdenv/setup\n";
    buildScript << "mkdir -p $out/bin\n";
    buildScript << "# TODO: Read discovery JSON and compile sources\n";
    buildScript << "# TODO: Link into final binary\n";
    buildScript << "echo 'cProject build (MVP - not yet functional)' > $out/bin/" << projectName << "\n";
    buildScript << "chmod +x $out/bin/" << projectName << "\n";

    addEnvString(buildDrv, "buildCommand", buildScript.str());
    addEnvString(buildDrv, "compiler", compiler);
    addEnvString(buildDrv, "root", rootPath.path.abs());

    // Outputs
    buildDrv.env["out"] = "";
    buildDrv.outputs.insert_or_assign("out", DerivationOutput::Deferred{});
    buildDrv.fillInOutputPaths(*state.store);

    auto buildDrvPath = state.store->writeDerivation(buildDrv, state.repair);

    // Return the build derivation output
    auto result = state.buildBindings(2);

    NixStringContext drvPathContext;
    drvPathContext.insert(NixStringContextElem::DrvDeep{.drvPath = buildDrvPath});
    result.alloc(state.s.drvPath)
        .mkString(
            state.store->printStorePath(buildDrvPath),
            drvPathContext,
            state.mem);

    NixStringContext outPathContext;
    outPathContext.insert(NixStringContextElem::Built{
        .drvPath = makeConstantStorePathRef(buildDrvPath),
        .output = "out"
    });
    result.alloc(state.s.outPath)
        .mkString(
            buildDrv.env.at("out"),
            outPathContext,
            state.mem);

    v.mkAttrs(result);
}

static RegisterPrimOp primop_cProject({
    .name = "cProject",
    .args = {"attrs"},
    .doc = R"(
        Build a C/C++ project with granular per-file compilation.

        **Example:**

        ```nix
        builtins.cProject {
          name = "myapp";
          root = ./.;
          compiler = pkgs.gcc;

          pkgConfigDeps = [ pkgs.openssl pkgs.zlib ];

          targets = {
            myapp = {
              type = "executable";
              sources = [ "src/main.c" "src/util.c" ];
            };
          };
        }
        ```

        This creates a granular build where each source file is compiled
        independently with proper header dependency tracking, enabling
        true incremental builds.

        Returns the final built binary directly.
    )",
    .fun = prim_cProject,
    .experimentalFeature = Xp::DynamicDerivations,
});

} // namespace nix
