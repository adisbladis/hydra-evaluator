#include <map>
#include <iostream>
#include <thread>

#include <nix/config.h>
#include <nix/shared.hh>
#include <nix/store-api.hh>
#include <nix/eval.hh>
#include <nix/eval-inline.hh>
#include <nix/util.hh>
#include <nix/get-drvs.hh>
#include <nix/globals.hh>
#include <nix/common-eval-args.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/attr-path.hh>
#include <nix/derivations.hh>
#include <nix/local-fs-store.hh>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

using namespace nix;

static Path gcRootsDir;

struct MyArgs : MixEvalArgs, MixCommonArgs
{
    Path releaseExpr;
    bool flake = false;
    bool dryRun = false;
    size_t nrWorkers = 1;
    size_t maxMemorySize = 4096;

    MyArgs() : MixCommonArgs("hydra-eval-jobs")
    {
        addFlag({
            .longName = "help",
            .description = "show usage information",
            .handler = {[&]() {
                printHelp(programName, std::cout);
                throw Exit();
            }}
        });

        addFlag({
            .longName = "gc-roots-dir",
            .description = "garbage collector roots directory",
            .labels = {"path"},
            .handler = {&gcRootsDir}
        });

        addFlag({
            .longName = "workers",
            .description = "number of evaluate workers",
            .labels = {"workers"},
            .handler = {[=](std::string s) {
                nrWorkers = std::stoi(s);
            }}
        });

        addFlag({
            .longName = "max-memory-size",
            .description = "maximum evaluation memory size",
            .labels = {"size"},
            .handler = {[=](std::string s) {
                maxMemorySize = std::stoi(s);
            }}
        });

        addFlag({
            .longName = "dry-run",
            .description = "don't create store derivations",
            .handler = {&dryRun, true}
        });

        addFlag({
            .longName = "flake",
            .description = "build a flake",
            .handler = {&flake, true}
        });

        expectArg("expr", &releaseExpr);
    }
};

static MyArgs myArgs;

static std::string queryMetaStrings(EvalState & state, DrvInfo & drv, const string & name, const string & subAttribute)
{
    Strings res;
    std::function<void(Value & v)> rec;

    rec = [&](Value & v) {
        state.forceValue(v);
        if (v.type == tString)
            res.push_back(v.string.s);
        else if (v.isList())
            for (unsigned int n = 0; n < v.listSize(); ++n)
                rec(*v.listElems()[n]);
        else if (v.type == tAttrs) {
            auto a = v.attrs->find(state.symbols.create(subAttribute));
            if (a != v.attrs->end())
                res.push_back(state.forceString(*a->value));
        }
    };

    Value * v = drv.queryMeta(name);
    if (v) rec(*v);

    return concatStringsSep(", ", res);
}

static void worker(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    Value vTop;

    if (myArgs.flake) {
        using namespace flake;

        auto flakeRef = parseFlakeRef(myArgs.releaseExpr);

        auto vFlake = state.allocValue();

        auto lockedFlake = lockFlake(state, flakeRef,
            LockFlags {
                .updateLockFile = false,
                .useRegistries = false,
                .allowMutable = false,
            });

        callFlake(state, lockedFlake, *vFlake);

        auto vOutputs = vFlake->attrs->get(state.symbols.create("outputs"))->value;
        state.forceValue(*vOutputs);

        auto aHydraJobs = vOutputs->attrs->get(state.symbols.create("hydraJobs"));
        if (!aHydraJobs)
            aHydraJobs = vOutputs->attrs->get(state.symbols.create("checks"));
        if (!aHydraJobs)
            throw Error("flake '%s' does not provide any Hydra jobs or checks", flakeRef);

        vTop = *aHydraJobs->value;

    } else {
        state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);
    }

    auto vRoot = state.allocValue();
    state.autoCallFunction(autoArgs, vTop, *vRoot);

    while (true) {
        /* Wait for the master to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        std::string attrPath(s, 3);

        debug("worker process %d at '%s'", getpid(), attrPath);

        /* Evaluate it and send info back to the master. */
        nlohmann::json reply;

        try {
            auto vTmp = findAlongAttrPath(state, attrPath, autoArgs, *vRoot).first;

            auto v = state.allocValue();
            state.autoCallFunction(autoArgs, *vTmp, *v);

            if (auto drv = getDerivation(state, *v, false)) {

                DrvInfo::Outputs outputs = drv->queryOutputs();

                if (drv->querySystem() == "unknown")
                    throw EvalError("derivation must have a 'system' attribute");

                auto drvPath = drv->queryDrvPath();

                nlohmann::json job;

                job["drvPath"] = drvPath;

                /* Register the derivation as a GC root.  !!! This
                   registers roots for jobs that we may have already
                   done. */
                auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();
                if (gcRootsDir != "" && localStore) {
                    Path root = gcRootsDir + "/" + std::string(baseNameOf(drvPath));
                    if (!pathExists(root))
                        localStore->addPermRoot(localStore->parseStorePath(drvPath), root);
                }

                reply["job"] = std::move(job);
            }

            else if (v->type == tAttrs) {
                auto attrs = nlohmann::json::array();
                StringSet ss;
                for (auto & i : v->attrs->lexicographicOrder()) {
                    std::string name(i->name);
                    if (name.find('.') != std::string::npos || name.find(' ') != std::string::npos) {
                        printError("skipping job with illegal name '%s'", name);
                        continue;
                    }
                    attrs.push_back(name);
                }
                reply["attrs"] = std::move(attrs);
            }

            else if (v->type == tNull)
                ;

            else throw TypeError("attribute '%s' is %s, which is not supported", attrPath, showType(*v));

        } catch (EvalError & e) {
            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(e.msg(), true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError("error: %s", reply["error"]);
        }

        writeLine(to.get(), reply.dump());

        /* If our RSS exceeds the maximum, exit. The master will
           start a new process. */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        if ((size_t) r.ru_maxrss > myArgs.maxMemorySize * 1024) break;
    }

    writeLine(to.get(), "restart");
}

int main(int argc, char * * argv)
{
    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH");

    return handleExceptions(argv[0], [&]() {
        initNix();
        initGC();

        myArgs.parseCmdline(argvToStrings(argc, argv));

        /* FIXME: The build hook in conjunction with import-from-derivation is causing "unexpected EOF" during eval */
        settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        evalSettings.restrictEval = true;

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        evalSettings.pureEval = myArgs.flake;

        if (myArgs.dryRun) settings.readOnlyMode = true;

        if (myArgs.releaseExpr == "") throw UsageError("no expression specified");

        if (gcRootsDir == "") printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        struct State
        {
            std::set<std::string> todo{""};
            std::set<std::string> active;
            nlohmann::json jobs;
            std::exception_ptr exc;
        };

        std::condition_variable wakeup;

        Sync<State> state_;

        /* Start a handler thread per worker process. */
        auto handler = [&]()
        {
            try {
                pid_t pid = -1;
                AutoCloseFD from, to;

                while (true) {

                    /* Start a new worker process if necessary. */
                    if (pid == -1) {
                        Pipe toPipe, fromPipe;
                        toPipe.create();
                        fromPipe.create();
                        pid = startProcess(
                            [&,
                             to{std::make_shared<AutoCloseFD>(std::move(fromPipe.writeSide))},
                             from{std::make_shared<AutoCloseFD>(std::move(toPipe.readSide))}
                            ]()
                            {
                                try {
                                    EvalState state(myArgs.searchPath, openStore());
                                    Bindings & autoArgs = *myArgs.getAutoArgs(state);
                                    worker(state, autoArgs, *to, *from);
                                } catch (std::exception & e) {
                                    nlohmann::json err;
                                    err["error"] = e.what();
                                    writeLine(to->get(), err.dump());
                                    // Don't forget to print it into the STDERR log, this is
                                    // what's shown in the Hydra UI.
                                    printError("error: %s", err["error"]);
                                }
                            },
                            ProcessOptions { .allowVfork = false });
                        from = std::move(fromPipe.readSide);
                        to = std::move(toPipe.writeSide);
                        debug("created worker process %d", pid);
                    }

                    /* Check whether the existing worker process is still there. */
                    auto s = readLine(from.get());
                    if (s == "restart") {
                        pid = -1;
                        continue;
                    } else if (s != "next") {
                        auto json = nlohmann::json::parse(s);
                        throw Error("worker error: %s", (std::string) json["error"]);
                    }

                    /* Wait for a job name to become available. */
                    std::string attrPath;

                    while (true) {
                        checkInterrupt();
                        auto state(state_.lock());
                        if ((state->todo.empty() && state->active.empty()) || state->exc) {
                            writeLine(to.get(), "exit");
                            return;
                        }
                        if (!state->todo.empty()) {
                            attrPath = *state->todo.begin();
                            state->todo.erase(state->todo.begin());
                            state->active.insert(attrPath);
                            break;
                        } else
                            state.wait(wakeup);
                    }

                    /* Tell the worker to evaluate it. */
                    writeLine(to.get(), "do " + attrPath);

                    /* Wait for the response. */
                    auto response = nlohmann::json::parse(readLine(from.get()));

                    /* Handle the response. */
                    StringSet newAttrs;

                    if (response.find("job") != response.end()) {
                        auto state(state_.lock());
                        state->jobs[attrPath] = response["job"];
                    }

                    if (response.find("attrs") != response.end()) {
                        for (auto & i : response["attrs"]) {
                            auto s = (attrPath.empty() ? "" : attrPath + ".") + (std::string) i;
                            newAttrs.insert(s);
                        }
                    }

                    if (response.find("error") != response.end()) {
                        auto state(state_.lock());
                        state->jobs[attrPath]["error"] = response["error"];
                    }

                    /* Add newly discovered job names to the queue. */
                    {
                        auto state(state_.lock());
                        state->active.erase(attrPath);
                        for (auto & s : newAttrs)
                            state->todo.insert(s);
                        wakeup.notify_all();
                    }
                }
            } catch (...) {
                auto state(state_.lock());
                state->exc = std::current_exception();
                wakeup.notify_all();
            }
        };

        std::vector<std::thread> threads;
        for (size_t i = 0; i < myArgs.nrWorkers; i++)
            threads.emplace_back(std::thread(handler));

        for (auto & thread : threads)
            thread.join();

        auto state(state_.lock());

        if (state->exc)
            std::rethrow_exception(state->exc);

        std::cout << state->jobs.dump(2) << "\n";
    });
}
