// Disassembles and creates a function at every address listed (hex, one per line)
// in the file given as the first script argument.
//@category Hino
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import java.nio.file.*;
import java.util.*;

public class CreateFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> lines = Files.readAllLines(Paths.get(getScriptArgs()[0]));
        int made = 0;
        for (String l : lines) {
            l = l.trim();
            if (l.isEmpty()) continue;
            Address a = toAddr(Long.parseLong(l.replace("0x", ""), 16));
            if (getFunctionAt(a) != null) continue;
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (createFunction(a, null) != null) made++;
        }
        println("created " + made + " functions");
    }
}
