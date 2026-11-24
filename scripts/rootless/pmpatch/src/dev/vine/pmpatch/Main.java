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
import java.util.List;
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
 * overlay gives it, and its capabilities through VineEthernet. service-wifi.jar: WifiServiceImpl's Wi-Fi state and
 * connection info through VineWifi. A hook (hook/) goes in as one more
 * classesN.dex. Exits 2, writing nothing, when IN is none of these or is
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
            byte[] patched = null;
            String patchedName = null;
            Patch patch = null;
            for (int d = 0; d < files.size() && patched == null; d++) {
                for (ClassDef c : files.get(d).getClasses()) {
                    for (Patch p : PATCHES) {
                        if (patched == null && c.getType().equals(p.type)) {
                            patched = rewrite(files.get(d), p);
                            patchedName = dexes.get(d);
                            patch = p;
                        }
                    }
                }
            }
            if (patched == null) fail("nothing to patch");
            if (edits == 0 || patch.hook != null && redirected.isEmpty()) fail("no call to redirect in " + patch.type);
            byte[] hook = patch.hook == null ? null : read(Main.class.getResourceAsStream("/" + patch.hook + ".dex"));
            if (hook != null) {
                DexBackedDexFile hookDex = new DexBackedDexFile(Opcodes.forApi(33), hook);
                for (MethodReference ref : redirected) {
                    if (!declares(hookDex, ref)) fail("the hook lacks " + ref);
                }
            }
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(args[1]))) {
                long at = 0;
                for (Enumeration<? extends ZipEntry> e = in.entries(); e.hasMoreElements(); ) {
                    ZipEntry entry = e.nextElement();
                    byte[] data = entry.getName().equals(patchedName) ? patched : read(in.getInputStream(entry));
                    at = put(out, at, entry.getName(), data);
                    if (hook != null && entry.getName().equals(dexes.get(dexes.size() - 1))) {
                        at = put(out, at, dexName(dexes.size() + 1), hook);
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

    /** `dex` with the patch's class's methods edited */
    static byte[] rewrite(DexBackedDexFile dex, Patch patch) throws IOException {
        DexPool pool = new DexPool(dex.getOpcodes());
        for (ClassDef c : dex.getClasses()) {
            if (!c.getType().equals(patch.type)) {
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

    /** In WifiServiceImpl's getWifiEnabledState, getConnectionInfo, getScanResults and startScan */
    static boolean redirectWifi(Method m, MutableMethodImplementation impl) {
        if (m.getName().equals("getWifiEnabledState") && m.getParameterTypes().isEmpty())
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/ActiveModeWarden;", "getWifiState");
        if (m.getName().equals("getConnectionInfo") || m.getName().equals("getScanResults")
                || m.getName().equals("startScan"))
            return redirect(impl, PATCHES[2], "Lcom/android/server/wifi/WifiThreadRunner;", "call");
        return false;
    }

    /**
     * Each virtual or interface call of owner.name made a static call of
     * the hook's method of that name, the receiver its first parameter
     */
    static boolean redirect(MutableMethodImplementation impl, Patch patch, String owner, String name) {
        boolean changed = false;
        List<BuilderInstruction> insns = impl.getInstructions();
        for (int i = 0; i < insns.size(); i++) {
            BuilderInstruction insn = insns.get(i);
            Opcode op = insn.getOpcode();
            boolean range = op == Opcode.INVOKE_INTERFACE_RANGE || op == Opcode.INVOKE_VIRTUAL_RANGE;
            if (!range && op != Opcode.INVOKE_INTERFACE && op != Opcode.INVOKE_VIRTUAL) continue;
            Object r = ((ReferenceInstruction) insn).getReference();
            if (!(r instanceof MethodReference) || !((MethodReference) r).getDefiningClass().equals(owner)
                    || !((MethodReference) r).getName().equals(name))
                continue;
            MethodReference ref = (MethodReference) r;
            List<CharSequence> params = new ArrayList<>();
            params.add(owner);
            params.addAll(ref.getParameterTypes());
            MethodReference hook = new ImmutableMethodReference(hookType(patch), name, params, ref.getReturnType());
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
