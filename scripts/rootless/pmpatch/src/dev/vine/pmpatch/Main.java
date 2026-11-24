package dev.vine.pmpatch;

import com.android.tools.smali.dexlib2.Opcode;
import com.android.tools.smali.dexlib2.Opcodes;
import com.android.tools.smali.dexlib2.builder.BuilderInstruction;
import com.android.tools.smali.dexlib2.builder.MutableMethodImplementation;
import com.android.tools.smali.dexlib2.builder.instruction.BuilderInstruction3rc;
import com.android.tools.smali.dexlib2.dexbacked.DexBackedDexFile;
import com.android.tools.smali.dexlib2.iface.ClassDef;
import com.android.tools.smali.dexlib2.iface.Method;
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction3rc;
import com.android.tools.smali.dexlib2.iface.reference.MethodReference;
import com.android.tools.smali.dexlib2.immutable.ImmutableClassDef;
import com.android.tools.smali.dexlib2.immutable.ImmutableMethod;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference;
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
 * Main IN.jar OUT.jar: the guest's services.jar with ComputerEngine's
 * AppsFilterSnapshot.shouldFilterApplication calls (package queries and
 * intent resolution) made through VineAppsHide (hook/, which goes in as
 * one more classesN.dex). Exits 2, writing nothing, when IN has no such
 * call or already has the hook.
 */
public final class Main {
    static final String ENGINE = "Lcom/android/server/pm/ComputerEngine;";
    static final String FILTER = "Lcom/android/server/pm/AppsFilterSnapshot;";
    static final String HOOK = "Lcom/android/server/pm/VineAppsHide;";
    static final String NAME = "shouldFilterApplication";

    public static void main(String[] args) throws IOException {
        if (args.length != 2) {
            System.err.println("usage: Main IN.jar OUT.jar");
            System.exit(1);
        }
        try (ZipFile in = new ZipFile(args[0])) {
            List<String> dexes = new ArrayList<>();
            for (int i = 1; in.getEntry(dexName(i)) != null; i++) dexes.add(dexName(i));
            byte[] patched = null;
            String patchedName = null;
            List<DexBackedDexFile> files = new ArrayList<>();
            for (String name : dexes) {
                DexBackedDexFile dex = new DexBackedDexFile(Opcodes.forApi(33), read(in.getInputStream(in.getEntry(name))));
                for (ClassDef c : dex.getClasses()) {
                    if (c.getType().equals(HOOK)) fail("already patched");
                }
                files.add(dex);
            }
            for (int d = 0; d < files.size(); d++) {
                DexBackedDexFile dex = files.get(d);
                String name = dexes.get(d);
                for (ClassDef c : dex.getClasses()) {
                    if (c.getType().equals(ENGINE)) {
                        patched = rewrite(dex, c);
                        patchedName = name;
                        break;
                    }
                }
                if (patched != null) break;
            }
            if (patched == null) fail(ENGINE + " not found");
            byte[] hook = read(Main.class.getResourceAsStream("/hook.dex"));
            if (!declares(new DexBackedDexFile(Opcodes.forApi(33), hook), redirected)) {
                fail("the hook lacks " + redirected);
            }
            try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(args[1]))) {
                long at = 0;
                for (Enumeration<? extends ZipEntry> e = in.entries(); e.hasMoreElements(); ) {
                    ZipEntry entry = e.nextElement();
                    byte[] data = entry.getName().equals(patchedName) ? patched : read(in.getInputStream(entry));
                    at = put(out, at, entry.getName(), data);
                    if (entry.getName().equals(dexes.get(dexes.size() - 1))) {
                        at = put(out, at, dexName(dexes.size() + 1), hook);
                    }
                }
            }
        }
    }

    static int calls(Method m) {
        int n = 0;
        for (com.android.tools.smali.dexlib2.iface.instruction.Instruction insn : m.getImplementation().getInstructions()) {
            if (insn.getOpcode() == Opcode.INVOKE_INTERFACE_RANGE && isFilter(((Instruction3rc) insn).getReference())) n++;
        }
        return n;
    }

    static boolean isFilter(Object ref) {
        return ref instanceof MethodReference && ((MethodReference) ref).getDefiningClass().equals(FILTER)
                && ((MethodReference) ref).getName().equals(NAME);
    }

    /** The call rewrite() made, which the hook must declare */
    static MethodReference redirected;

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

    /** Every call, all with the same descriptor */
    static byte[] rewrite(DexBackedDexFile dex, ClassDef engine) throws IOException {
        List<Method> methods = new ArrayList<>();
        int calls = 0;
        for (Method m : engine.getMethods()) {
            if (m.getImplementation() != null && calls(m) > 0) {
                MutableMethodImplementation impl = new MutableMethodImplementation(m.getImplementation());
                List<BuilderInstruction> insns = impl.getInstructions();
                for (int i = 0; i < insns.size(); i++) {
                    BuilderInstruction insn = insns.get(i);
                    if (insn.getOpcode() != Opcode.INVOKE_INTERFACE_RANGE) continue;
                    if (!isFilter(((Instruction3rc) insn).getReference())) continue;
                    MethodReference ref = (MethodReference) ((Instruction3rc) insn).getReference();
                    List<CharSequence> params = new ArrayList<>();
                    params.add(FILTER);
                    params.addAll(ref.getParameterTypes());
                    Instruction3rc call = (Instruction3rc) insn;
                    redirected = new ImmutableMethodReference(HOOK, NAME, params, ref.getReturnType());
                    impl.replaceInstruction(i, new BuilderInstruction3rc(Opcode.INVOKE_STATIC_RANGE,
                            call.getStartRegister(), call.getRegisterCount(), redirected));
                    calls++;
                }
                methods.add(new ImmutableMethod(m.getDefiningClass(), m.getName(), m.getParameters(), m.getReturnType(),
                        m.getAccessFlags(), m.getAnnotations(), m.getHiddenApiRestrictions(), impl));
            } else {
                methods.add(m);
            }
        }
        if (calls == 0) fail("no calls of " + FILTER + NAME);
        DexPool pool = new DexPool(dex.getOpcodes());
        for (ClassDef c : dex.getClasses()) {
            if (c.getType().equals(ENGINE)) {
                pool.internClass(new ImmutableClassDef(c.getType(), c.getAccessFlags(), c.getSuperclass(),
                        c.getInterfaces(), c.getSourceFile(), c.getAnnotations(), c.getFields(), methods));
            } else {
                pool.internClass(c);
            }
        }
        MemoryDataStore store = new MemoryDataStore();
        pool.writeTo(store);
        return Arrays.copyOf(store.getBuffer(), store.getSize());
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
