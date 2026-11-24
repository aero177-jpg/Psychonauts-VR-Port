import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

// Args: outfile hexaddr needle1 [needle2 ...]
// Decompiles every function that references hexaddr and prints the lines
// containing any needle (with the function name), so a field access through a
// global pointer can be found without reading every function.
public class RefsGrep extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        Address target = toAddr(Long.parseLong(a[1], 16));
        Set<Function> funcs = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(target)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) funcs.add(f);
        }
        out.println("functions referencing " + a[1] + ": " + funcs.size());
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (Function f : funcs) {
            DecompileResults dr = di.decompileFunction(f, 60, monitor);
            if (!dr.decompileCompleted()) continue;
            String[] lines = dr.getDecompiledFunction().getC().split("\n");
            List<String> hits = new ArrayList<>();
            for (String line : lines)
                for (int i = 2; i < a.length; i++)
                    if (line.contains(a[i])) { hits.add(line.trim()); break; }
            if (!hits.isEmpty()) {
                out.println("== " + f.getName() + " @" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                for (String h : hits) out.println("   " + h);
            }
        }
        out.close();
    }
}
