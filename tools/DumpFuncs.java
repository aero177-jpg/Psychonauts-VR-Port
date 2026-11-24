import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

// Args: outfile addr1 addr2 ...   (hex, e.g. 5EDF50)
// For each: decompile the function containing addr and list its callers.
public class DumpFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address ad = toAddr(Long.parseLong(a[i], 16));
            Function f = getFunctionContaining(ad);
            if (f == null) f = createFunction(ad, null);
            out.println("==== " + a[i] + " -> " + (f == null ? "NO FUNCTION" : f.getName() + " @" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses()));
            if (f == null) continue;
            StringBuilder cs = new StringBuilder("callers:");
            int n = 0;
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                if (n++ > 12) break;
                cs.append(' ').append(r.getFromAddress());
            }
            out.println(cs);
            DecompileResults dr = di.decompileFunction(f, 60, monitor);
            if (dr.decompileCompleted()) out.println(dr.getDecompiledFunction().getC());
            else out.println("decompile failed: " + dr.getErrorMessage());
        }
        out.close();
    }
}
