package dev.vine.pmpatch;

import com.android.tools.smali.dexlib2.Opcode;
import com.android.tools.smali.dexlib2.Opcodes;
import com.android.tools.smali.dexlib2.builder.BuilderInstruction;
import com.android.tools.smali.dexlib2.builder.MutableMethodImplementation;
import com.android.tools.smali.dexlib2.builder.instruction.BuilderInstruction21c;
import com.android.tools.smali.dexlib2.builder.instruction.BuilderInstruction35c;
import com.android.tools.smali.dexlib2.builder.instruction.BuilderInstruction3rc;
import com.android.tools.smali.dexlib2.dexbacked.DexBackedDexFile;
import com.android.tools.smali.dexlib2.iface.ClassDef;
import com.android.tools.smali.dexlib2.iface.Method;
import com.android.tools.smali.dexlib2.iface.instruction.OneRegisterInstruction;
import com.android.tools.smali.dexlib2.iface.instruction.ReferenceInstruction;
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction35c;
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction3rc;
import com.android.tools.smali.dexlib2.iface.reference.MethodReference;
import com.android.tools.smali.dexlib2.immutable.ImmutableClassDef;
import com.android.tools.smali.dexlib2.immutable.ImmutableMethod;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableStringReference;
import com.android.tools.smali.dexlib2.writer.io.MemoryDataStore;
import com.android.tools.smali.dexlib2.writer.pool.DexPool;

import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Main IN.jar OUT.jar: one of the guest's jars patched, whichever it is.
 * services.jar: ComputerEngine's AppsFilterSnapshot.shouldFilterApplication
 * calls (package queries and intent resolution) made through VineAppsHide.
 * service-connectivity.jar: the Ethernet network's legacy type name "WIFI",
 * as Wi-Fi's network agent names it, to go with the WIFI type the guest's
 * overlay gives it, and its capabilities through VineEthernet.
 * service-wifi.jar: WifiServiceImpl's Wi-Fi state, connection info and
 * scans, and ScanRequestProxy's failure broadcasts, through VineWifi. Each
 * hook (hook/) goes in as one more classesN.dex. Exits 2, writing nothing, when IN is none of these or is
 * already patched.
 */
public final class Main {
    static final String FILTER = "Lcom/android/server/pm/AppsFilterSnapshot;";
    static final String NAME = "shouldFilterApplication";

    interface Editor {
        /** Edits m's code in place; true if it changed */
        boolean edit(Method m, MutableMethodImplementation impl);
    }

    static final class Patch {
        final String type, hook;
        final Editor editor;

        /** `hook`: the hook's class name, its dex a resource of that name, or null */
        Patch(String type, String hook, Editor editor) {
            this.type = type;
            this.hook = hook;
            this.editor = editor;
        }
    }

    static final Patch[] PATCHES = {
        new Patch("Lcom/android/server/pm/ComputerEngine;", "VineAppsHide", Main::redirectFilter),
        new Patch("Lcom/android/server/ethernet/EthernetNetworkFactory$NetworkInterfaceState;", "VineEthernet",
                Main::patchEthernet),
        new Patch("Lcom/android/server/wifi/WifiServiceImpl;", "VineWifi", Main::redirectWifi),
        new Patch("Lcom/android/server/wifi/ScanRequestProxy;", "VineWifi", Main::redirectScanFailure),
    };

    /** The calls made to a hook, which it must declare */
    static final List<MethodReference> redirected = new ArrayList<>();
    static int edits;

    public static void main(String[] args) throws IOException {
        if (args.length != 2) {
            System.err.println("usage: Main IN.jar OUT.jar");
            System.exit(1);
        }
        try (ZipFile in = new ZipFile(args[0])) {
            List<String> dexes = new ArrayList<>();
            for (int i = 1; in.getEntry(dexName(i)) != null; i++) dexes.add(dexName(i));
            List<DexBackedDexFile> files = new ArrayList<>();
            for (String name : dexes) {
                DexBackedDexFile dex = new DexBackedDexFile(Opcodes.forApi(33), read(in.getInputStream(in.getEntry(name))));
                for (ClassDef c : dex.getClasses()) {
                    for (Patch p : PATCHES) {
                        if (p.hook != null && c.getType().equals(hookType(p))) fail("already patched");
                    }
                }
                files.add(dex);
            }
            // Every patch whose class the jar has, each dex rewritten once
            Map<String, byte[]> patched = new HashMap<>();
            Set<String> hooks = new LinkedHashSet<>();
            for (int d = 0; d < files.size(); d++) {
                List<Patch> here = new ArrayList<>();
                for (ClassDef c : files.get(d).getClasses()) {
                    for (Patch p : PATCHES) {
                        if (c.getType().equals(p.type)) here.add(p);
                    }
                }
                if (here.isEmpty()) continue;
                patched.put(dexes.get(d), rewrite(files.get(d), here));
                for (Patch p : here) {
                    if (p.hook != null) hooks.add(p.hook);
                }
            }
            if (patched.isEmpty()) fail("nothing to patch");
            if (edits == 0 || !hooks.isEmpty() && redirected.isEmpty()) fail("nothing to change");
            List<byte[]> hookDexes = new ArrayList<>();
            for (String h : hooks) hookDexes.add(read(Main.class.getResourceAsStream("/" + h + ".dex")));
            for (MethodReference ref : redirected) {
                boolean found = false;
                for (byte[] h : hookDexes) found |= declares(new DexBackedDexFile(Opcodes.forApi(33), h), ref);
                if (!found) fail("no hook declares " + ref);
            }
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(args[1]))) {
                long at = 0;
                for (Enumeration<? extends ZipEntry> e = in.entries(); e.hasMoreElements(); ) {
                    ZipEntry entry = e.nextElement();
                    byte[] data = patched.containsKey(entry.getName()) ? patched.get(entry.getName())
                            : read(in.getInputStream(entry));
                    at = put(out, at, entry.getName(), data);
                    if (entry.getName().equals(dexes.get(dexes.size() - 1))) {
                        for (int h = 0; h < hookDexes.size(); h++) {
                            at = put(out, at, dexName(dexes.size() + 1 + h), hookDexes.get(h));
                        }
                    }
                }
            }
        }
    }

    /** Hooks are in the patched class's package */
    static String hookType(Patch p) {
        return p.type.substring(0, p.type.lastIndexOf('/') + 1) + p.hook + ";";
    }


    static boolean declares(DexBackedDexFile dex, MethodReference ref) {
        for (ClassDef c : dex.getClasses()) {
            for (Method m : c.getMethods()) {
                if (c.getType().equals(ref.getDefiningClass()) && m.getName().equals(ref.getName())
                        && m.getReturnType().equals(ref.getReturnType())
                        && String.join(",", m.getParameterTypes()).equals(String.join(",", ref.getParameterTypes()))) {
                    return true;
                }
            }
        }
        return false;
    }

    static String dexName(int i) {
        return i == 1 ? "classes.dex" : "classes" + i + ".dex";
    }

    /** `dex` with the patches' classes' methods edited */
    static byte[] rewrite(DexBackedDexFile dex, List<Patch> patches) throws IOException {
        DexPool pool = new DexPool(dex.getOpcodes());
        for (ClassDef c : dex.getClasses()) {
            Patch patch = null;
            for (Patch p : patches) {
                if (c.getType().equals(p.type)) patch = p;
            }
            if (patch == null) {
                pool.internClass(c);
                continue;
            }
            List<Method> methods = new ArrayList<>();
            for (Method m : c.getMethods()) {
                MutableMethodImplementation impl =
                        m.getImplementation() == null ? null : new MutableMethodImplementation(m.getImplementation());
                if (impl != null && patch.editor.edit(m, impl)) {
                    methods.add(new ImmutableMethod(m.getDefiningClass(), m.getName(), m.getParameters(), m.getReturnType(),
                            m.getAccessFlags(), m.getAnnotations(), m.getHiddenApiRestrictions(), impl));
                } else {
                    methods.add(m);
                }
            }
            pool.internClass(new ImmutableClassDef(c.getType(), c.getAccessFlags(), c.getSuperclass(),
                    c.getInterfaces(), c.getSourceFile(), c.getAnnotations(), c.getFields(), methods));
        }
        MemoryDataStore store = new MemoryDataStore();
        pool.writeTo(store);
        return Arrays.copyOf(store.getBuffer(), store.getSize());
    }

    /** Every call of AppsFilterSnapshot.shouldFilterApplication */
    static boolean redirectFilter(Method m, MutableMethodImplementation impl) {
        return redirect(impl, PATCHES[0], FILTER, NAME);
    }

    /** The legacy type name, and the network agent's capabilities through the hook */
    static boolean patchEthernet(Method m, MutableMethodImplementation impl) {
        boolean renamed = renameEthernet(m, impl);
        boolean redirected = m.getName().equals("onIpLayerStarted") && redirect(impl, PATCHES[1],
                "Lcom/android/server/ethernet/EthernetNetworkFactory$Dependencies;", "makeEthernetNetworkAgent");
        return renamed || redirected;
    }

    /**
     * In WifiServiceImpl's getWifiEnabledState, getConnectionInfo, getScanResults, startScan,
     * getCurrentNetwork and getPrivilegedConfiguredNetworks
     */
    static boolean redirectWifi(Method m, MutableMethodImplementation impl) {
        if (m.getName().equals("getWifiEnabledState") && m.getParameterTypes().isEmpty())
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/ActiveModeWarden;", "getWifiState");
        if (m.getName().equals("getCurrentNetwork"))
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/ClientMode;", "syncGetCurrentNetwork");
        if (m.getName().equals("getPrivilegedConfiguredNetworks"))
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/WifiThreadRunner;", "call", "callConfigs");
        if (m.getName().equals("getConnectionInfo") || m.getName().equals("getScanResults")
                || m.getName().equals("startScan"))
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/WifiThreadRunner;", "call");
        return false;
    }

    /** ScanRequestProxy.startScan's failure broadcasts, which VineWifi's scans replace */
    static boolean redirectScanFailure(Method m, MutableMethodImplementation impl) {
        return m.getName().equals("startScan") && redirect(impl, PATCHES[3], "Lcom/android/server/wifi/ScanRequestProxy;",
                "sendScanResultFailureBroadcastToPackage");
    }

    /**
     * Each virtual, interface or direct call of owner.name made a static call of
     * the hook's method of that name, the receiver its first parameter
     */
    static boolean redirect(MutableMethodImplementation impl, Patch patch, String owner, String name) {
        return redirect(impl, patch, owner, name, name);
    }

    /** As above, to the hook's method `hookName` */
    static boolean redirect(MutableMethodImplementation impl, Patch patch, String owner, String name, String hookName) {
        boolean changed = false;
        List<BuilderInstruction> insns = impl.getInstructions();
        for (int i = 0; i < insns.size(); i++) {
            BuilderInstruction insn = insns.get(i);
            Opcode op = insn.getOpcode();
            boolean range = op == Opcode.INVOKE_INTERFACE_RANGE || op == Opcode.INVOKE_VIRTUAL_RANGE;
            if (!range && op != Opcode.INVOKE_INTERFACE && op != Opcode.INVOKE_VIRTUAL && op != Opcode.INVOKE_DIRECT) continue;
            Object r = ((ReferenceInstruction) insn).getReference();
            if (!(r instanceof MethodReference) || !((MethodReference) r).getDefiningClass().equals(owner)
                    || !((MethodReference) r).getName().equals(name))
                continue;
            MethodReference ref = (MethodReference) r;
            List<CharSequence> params = new ArrayList<>();
            params.add(owner);
            params.addAll(ref.getParameterTypes());
            MethodReference hook = new ImmutableMethodReference(hookType(patch), hookName, params, ref.getReturnType());
            redirected.add(hook);
            if (range) {
                Instruction3rc call = (Instruction3rc) insn;
                impl.replaceInstruction(i, new BuilderInstruction3rc(Opcode.INVOKE_STATIC_RANGE,
                        call.getStartRegister(), call.getRegisterCount(), hook));
            } else {
                Instruction35c call = (Instruction35c) insn;
                impl.replaceInstruction(i, new BuilderInstruction35c(Opcode.INVOKE_STATIC, call.getRegisterCount(),
                        call.getRegisterC(), call.getRegisterD(), call.getRegisterE(), call.getRegisterF(),
                        call.getRegisterG(), hook));
            }
            edits++;
            changed = true;
        }
        return changed;
    }

    /** const-string "Ethernet" right before NetworkAgentConfig.Builder.setLegacyTypeName */
    static boolean renameEthernet(Method m, MutableMethodImplementation impl) {
        boolean changed = false;
        List<BuilderInstruction> insns = impl.getInstructions();
        for (int i = 0; i + 1 < insns.size(); i++) {
            BuilderInstruction insn = insns.get(i), next = insns.get(i + 1);
            if (insn.getOpcode() != Opcode.CONST_STRING
                    || !"Ethernet".equals(((ReferenceInstruction) insn).getReference().toString())
                    || !(next instanceof ReferenceInstruction)
                    || !(((ReferenceInstruction) next).getReference() instanceof MethodReference)
                    || !((MethodReference) ((ReferenceInstruction) next).getReference()).getName().equals("setLegacyTypeName"))
                continue;
            impl.replaceInstruction(i, new BuilderInstruction21c(Opcode.CONST_STRING,
                    ((OneRegisterInstruction) insn).getRegisterA(), new ImmutableStringReference("WIFI")));
            edits++;
            changed = true;
        }
        return changed;
    }

    /**
     * Every entry stored, its data 4-byte aligned as zipalign leaves it, for
     * ART to map the dex files in place; `at` is the entry's offset
     */
    static long put(ZipOutputStream out, long at, String name, byte[] data) throws IOException {
        ZipEntry e = new ZipEntry(name);
        e.setTime(1199145600000L);
        CRC32 crc = new CRC32();
        crc.update(data);
        e.setMethod(ZipEntry.STORED);
        e.setSize(data.length);
        e.setCompressedSize(data.length);
        e.setCrc(crc.getValue());
        int header = 30 + name.getBytes("UTF-8").length;
        int pad = (int) ((4 - (at + header) % 4) % 4);
        if (pad > 0) e.setExtra(new byte[pad]);
        out.putNextEntry(e);
        out.write(data);
        out.closeEntry();
        return at + header + pad + data.length;
    }

    static byte[] read(InputStream in) throws IOException {
        if (in == null) throw new IOException("missing resource");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        in.close();
        return out.toByteArray();
    }

    static void fail(String why) {
        System.err.println("pmpatch: " + why);
        System.exit(2);
    }
}
