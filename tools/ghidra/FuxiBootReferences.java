// Locate the ABL boot handoff and framebuffer code in a locally supplied EFI.
// @category Fuxi
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;

public class FuxiBootReferences extends GhidraScript {
    @Override
    public void run() throws Exception {
        if (getScriptArgs().length != 1) {
            throw new IllegalArgumentException("Supply an output filename");
        }
        String[] queries = {"splash", "Splash", "framebuffer", "FrameBuffer",
            "ramdisk", "Ramdisk", "UpdateDeviceTree", "kernel_addr", "dtb_addr"};
        DecompInterface dec = new DecompInterface();
        Set<Address> done = new HashSet<>();
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0], "UTF-8")) {
            if (!dec.openProgram(currentProgram)) {
                throw new IllegalStateException(dec.getLastMessage());
            }
            out.println("Program: " + currentProgram.getName());
            out.println("SHA256: " + currentProgram.getExecutableSHA256());
            out.println("Language: " + currentProgram.getLanguageID());
            for (String query : queries) {
                out.println("\nQUERY " + query);
                for (java.nio.charset.Charset encoding : new java.nio.charset.Charset[] {
                        StandardCharsets.US_ASCII, StandardCharsets.UTF_16LE}) {
                    byte[] needle = query.getBytes(encoding);
                    Address cursor = currentProgram.getMinAddress();
                    for (int hits = 0; cursor != null && hits < 64; hits++) {
                        monitor.checkCancelled();
                        Address hit = currentProgram.getMemory().findBytes(
                            cursor, needle, null, true, monitor);
                        if (hit == null) break;
                        // The query may start inside a longer defined string.
                        ghidra.program.model.listing.Data data =
                            currentProgram.getListing().getDataContaining(hit);
                        Address start = data == null ? hit : data.getAddress();
                        out.println("  Match " + hit + " " + encoding +
                            (data == null ? "" : " " + data.getDefaultValueRepresentation()));
                        for (Reference ref : getReferencesTo(start)) {
                            Function fn = getFunctionContaining(ref.getFromAddress());
                            out.println("  Reference " + ref.getFromAddress() + " " +
                                (fn == null ? "no function" : fn.getName()));
                            if (fn != null && done.add(fn.getEntryPoint())) {
                                DecompileResults result = dec.decompileFunction(fn, 45, monitor);
                                out.println(result.decompileCompleted() &&
                                    result.getDecompiledFunction() != null ?
                                    result.getDecompiledFunction().getC() : result.getErrorMessage());
                            }
                        }
                        cursor = hit.next();
                    }
                }
            }
        } finally {
            dec.dispose();
        }
        println("Wrote " + getScriptArgs()[0]);
    }
}
