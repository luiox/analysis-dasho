import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * 逐类加载校验器：对 jar 内每个 .class 走一遍 Class.forName(name, false, loader)。
 *
 * 为什么不用 `java -jar` 启动：正常启动只校验实际执行到的路径，而改写帧的 pass
 * 出错时往往落在**没被执行到**的分支上。这里把每一个类都送进校验器（含死代码），
 * 布局/帧写错必然暴露。
 *
 * `initialize=false` 是刻意的：链接（含校验）在初始化之前完成，所以校验照样跑，
 * 但不会触发静态初始化——语料里有会开窗口、调 System.exit 的样本，初始化会挂住。
 *
 * 用法：java -Xverify:all VerifyJar.java <jar> [--quiet]
 * 输出：`classes=N ok=N failed=N`；失败明细打 stderr，退出码 = 1 表示有失败。
 */
public final class VerifyJar {

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java VerifyJar.java <jar> [--quiet]");
            System.exit(2);
        }
        File jar = new File(args[0]);
        boolean quiet = args.length > 1 && args[1].equals("--quiet");

        List<String> names = new ArrayList<>();
        try (ZipFile zf = new ZipFile(jar)) {
            Enumeration<? extends ZipEntry> en = zf.entries();
            while (en.hasMoreElements()) {
                String n = en.nextElement().getName();
                if (!n.endsWith(".class")) continue;
                if (n.startsWith("META-INF/")) continue;   // multi-release / 签名残留
                names.add(n.substring(0, n.length() - 6).replace('/', '.'));
            }
        }

        // 父 loader 置 null：只用这个 jar 自己的类，避免宿主机 classpath 兜底
        // 把本该失败的加载变成成功。
        URLClassLoader cl = new URLClassLoader(new URL[] {jar.toURI().toURL()}, null);

        int ok = 0;
        List<String> failed = new ArrayList<>();
        for (String n : names) {
            try {
                Class.forName(n, false, cl);
                ok++;
            } catch (Throwable t) {
                failed.add(n);
                if (!quiet) {
                    System.err.println("FAIL " + n);
                    System.err.println("     " + t.getClass().getName() + ": " + t.getMessage());
                    StackTraceElement[] st = t.getStackTrace();
                    for (int i = 0; i < Math.min(3, st.length); i++) {
                        System.err.println("       at " + st[i]);
                    }
                }
            }
        }
        cl.close();

        System.out.println("classes=" + names.size() + " ok=" + ok + " failed=" + failed.size());
        if (!quiet && !failed.isEmpty()) {
            System.out.println("failed: " + String.join(", ", failed));
        }
        System.exit(failed.isEmpty() ? 0 : 1);
    }
}
