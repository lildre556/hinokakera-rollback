// Decompiles every function (or only those named in args) and writes the C to
// the file given as the first script argument, one block per function, sorted by address.
//@category KOFMod
import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.util.task.TaskMonitor;
import java.io.*;
import java.util.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        Set<Long> only = new HashSet<>();
        for (int i = 1; i < args.length; i++) {
            only.add(Long.parseUnsignedLong(args[i].replace("0x", ""), 16));
        }
        List<Function> funcs = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isThunk() || f.isExternal()) continue;
            if (!only.isEmpty() && !only.contains(f.getEntryPoint().getOffset())) continue;
            funcs.add(f);
        }
        println("decompiling " + funcs.size() + " functions");
        DecompileConfigurer cfg = d -> {
            DecompileOptions o = new DecompileOptions();
            o.grabFromProgram(currentProgram);
            d.setOptions(o);
            d.toggleCCode(true);
            d.toggleSyntaxTree(false);
            d.setSimplificationStyle("decompile");
        };
        DecompilerCallback<String[]> cb = new DecompilerCallback<String[]>(currentProgram, cfg) {
            @Override
            public String[] process(DecompileResults r, TaskMonitor m) {
                Function f = r.getFunction();
                String c = r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage() + "\n";
                return new String[] { String.format("%016x", f.getEntryPoint().getOffset()), "// ==== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + "\n" + c };
            }
        };
        cb.setTimeout(60);
        List<String[]> results = ParallelDecompiler.decompileFunctions(cb, funcs, monitor);
        cb.dispose();
        results.sort(Comparator.comparing(a -> a[0]));
        try (Writer w = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(out), "UTF-8"))) {
            for (String[] r : results) w.write(r[1] + "\n");
        }
        println("wrote " + results.size() + " functions to " + out);
    }
}
