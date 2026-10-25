# DecryptLib 技术说明(资源加密运行时库)

本文是 DashO 12.8 资源加密运行时库 `lib/DecryptLib.jar`(20KB,19 类)的逐类勘察与算法级干净重写。重写原则:只做算法重写,不复制原始字节码;错误消息串不复刻,以清晰英文替代。

## 1. 定位与触发条件

DecryptLib 是 DashO **资源加密(Resource Encryption)** 的运行时解密库。构建时被选中的资源文件被加密写入 JAR;运行期受保护代码调用 `Wrapper.decryptWithIOException(...)` / `decryptWithRuntimeException(...)` 拿到透明解密的 `InputStream`。

- **字符串加密不注入本库**:`level` × `implementations` 的任意组合下都未观察到注入——字符串加密自带解密器(`{Owner}$N` 匿名类),与本库无关(见 [string-encryption.md](string-encryption.md))。
- 注入条件:Android 资源加密(`<resourceencryption>`);Standard Mode(标准 Java JAR)下该配置元素被忽略(官方文档注记)。
- 库自身也被 DashO 字符串加密+opaque 谓词混淆(`dasho/e`、`dasho/c`),与受保护应用用同一套模板——分析时先解库内字符串再看逻辑。

## 2. 类清单(19 类)

| 内部名 | 角色 |
|--------|------|
| `com/preemptive/dasho/decryptlib/Wrapper` | 公开门面:2 个 decryptWith* + 私有 `a(InputStream)` |
| `Wrapper$Secrets` | 静态注入点:`scheme: String`、`secretKey: String`(构建时写入) |
| `com/preemptive/dasho/cryptlib/DecryptionException` | 受检异常(extends Exception) |
| `dasho/a` | 密码器门面:JCE Cipher + SecretKey + 头校验 + 调试监听 |
| `dasho/b` (extends InputStream) | 解密流包装:缓冲、按需解块、mark/reset |
| `dasho/b$b` | 头记录:完整头字节 `byte[] a` + IV 偏移 `int b` |
| `dasho/b$1` / `$a` / `$b` | read/skip 内部回调 |
| `dasho/l` (abstract) | 自定义编解码引擎基类(Base64 变体) |
| `dasho/k` (extends l) | 编解码实现:6-bit 打包、双字母表、转义 |
| `dasho/l$a` | 编解码状态机上下文 |
| `dasho/h` (Enum×2) | 字母表方向(名加密,推测 FORWARD/REVERSE) |
| `dasho/c` | 库内字符串解密器 B(getChars/onsite) |
| `dasho/e` | 库内字符串解密器 A(lastIndexOf)+ opaque 源 |
| `dasho/m` | UTF-8 编解码辅助 |
| `dasho/d` | 调试监听接口(`a(String)V`),静态字段 `dasho/a.a` 可选注入 |
| `dasho/f,g,i,j` | 空接口/标记 |

## 3. 调用链(自顶向下)

```
Wrapper.decryptWithIOException(InputStream)
  └─ Wrapper.a(InputStream)                    // 读 Secrets.scheme / Secrets.secretKey
       ├─ [可选] dasho/a.a(dasho/d).a(...)     // 调试回显 scheme/key
       └─ dasho/b.a(InputStream, scheme, key)  // 工厂
            ├─ dasho/b.a(InputStream)          // 包一层 BufferedInputStream?(短方法)
            ├─ dasho/b.b(InputStream, scheme, key)
            │    ├─ b$b hdr = b.b(InputStream)  // 解析+校验头(见 §4)
            │    ├─ new dasho/a(scheme, key)    // JCE 初始化(见 §5)
            │    ├─ 校验 cipher.getBlockSize()==16
            │    ├─ is.skip(hdr.b + 16)        // 跳过头+IV
            │    ├─ [可选] 监听器回显
            │    └─ cryptor.c(hdr.a, hdr.b)     // init(DECRYPT, key, IV)
            └─ return new dasho/b(is, cryptor)  // 解密流
```

## 4. 流头格式

`dasho/b.b(InputStream)` 读最多 37 字节(mark 74)后:

```
偏移  长度      内容
0     4         混淆字序 checksum(见下)
4     varint    变长字段:从 off=4 起逐字节,直到某字节 bit5(0x20) 置位;
                最多扫 17 字节(参数 16+1)。计数=字段占用字节数 n。
4+n   16        IV(AES block size)
4+n+16 .. EOF   密文(AES/CBC/PKCS5Padding)
```

**checksum 算法**(`dasho/a.a([BI)I`,读 off..off+3):

```
value = (buf[off+1]<<24 & 0xFF000000)
      | (buf[off+3]<<16 & 0x00FF0000)
      | (buf[off]  <<  8 & 0x0000FF00)
      | (buf[off+2]      & 0x000000FF)
// 字节序打乱:逻辑顺序 [1,3,0,2] ← 存储顺序 [0,1,2,3]
```

**完整性校验**(`dasho/a.a([BII[BII)I`,多项式哈希):

```
h = 1776651266                          // seed(十进制,勿转 hex 抄错)
for i in [off1, off1+len1): h = h*31 + buf1[i]
for i in [off2, off2+len2): h = h*31 + buf2[i]
// 调用: a.a(hdrBytes, 4, n, hdrBytes, 4+n, 16)
// 即哈希覆盖 varint 字段 + IV
if (checksum_at_0 != h) → 返回 null(非加密流或损坏)
```

**varint 计数**(`dasho/a.b([BI)I`,消费端 `dasho/b.b`):纯扫描计数,不解码数值。**载荷值语义为空(实测确认)**:消费端 `dasho/b.b(InputStream,String,String)` 构造密码器后 `skip(hdr.b + 16)`(`b$b` 构造传 `4 + varintLen`,即 `hdr.b = 4 + varintLen`,跳过量 `= 4 + varintLen + 16`,从密文起点解密);varint **解码值**从未传给 Cipher 或任何长度校验——只有 varint **字节数**影响布局(定位 IV)。即 varint 是**纯门/标记**,其字节参与 checksum 自校验(见上文完整性校验),值本身语义为空。干净重写可固定为 1 字节或直接去掉,但要与既有加密资源互通则必须保留扫描逻辑(varint 字节影响 checksum,改变布局会使存量资源校验失败)。

## 5. 密钥与算法(dasho/a)

```
scheme   = Wrapper$Secrets.scheme      // 例:"AES/CBC/PKCS5Padding"
secretKey= Wrapper$Secrets.secretKey   // Base64 文本(自定义字母表!)
```

构造 `dasho/a(String scheme, String keyMaterial)`:

1. `dasho/a.a(String)I` = `scheme.substring(0, scheme.indexOf('/'))` → "AES"
2. `Cipher.getInstance(scheme)` → 字段 `c`
3. `dasho/k.b = new dasho/k()`(无参)
4. `keyBytes = dasho/k.b(keyMaterial)` → **自定义 Base64 解码**(见 §6)
5. `SecretKeySpec(keyBytes, "AES")` → 字段 `d`
6. 失败路径包 `DecryptionException`(字符串加密,消息不必复刻)

`dasho/a.c([BI)V` — 初始化解密:

```
IvParameterSpec iv = new IvParameterSpec(buf, off, cipher.getBlockSize()) // 16
cipher.init(Cipher.DECRYPT_MODE /*2*/, key, iv)
```

`dasho/a` 无加密模式公开路径——本库**只做解密**;加密在构建器(DashOPro.jar)内完成。

**块大小硬校验**:`b(InputStream,String,String)` 中 `if (cryptor.a() != 16) throw`;即资源加密固定 AES-128/CBC(16 字节块)。scheme 字符串可含 CBC/CTR 等,但运行时只放行 blockSize==16。

## 6. 自定义 Base64(dasho/k + dasho/l)

`dasho/k` 实现 6-bit 打包编解码,静态表在 `dasho/k.<clinit>` 中,内容如下:

| 表 | 内容 |
|----|------|
| `j`(64B) | `ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/` —— **标准 RFC4648 字母表** |
| `k`(64B) | `ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_` —— **URL-safe 变体** |
| `l`(123B) | 解码查找表 char→6bit:同时接受 `+/` 与 `-_`(43→62, 45→62, 47→63, 95→63;A-Z→0-25, a-z→26-51, 0-9→52-61;其余 -1) |

**结论**:解码路径是**宽容双字母表 Base64**——任何同时含 `+/` 或 `-_` 的输入都能正确解码;编码路径由布尔 flag 选 `j`(标准)或 `k`(URL-safe)。secretKey 实际就是标准(或 URL-safe)Base64 文本,无需特殊提取。`dasho/h` Enum 两值 = 编码方向开关。

核心 `k.a([BII, l$a)V`(251 指令):

```
对每个输入字节 b:
  if b == escape('='): 标记 done, break
  v = table_l[b]                    // 6-bit 值(-1=非法,跳过)
  state.h = (state.h + 1) % 4
  state.a = (state.a << 6) + v      // 累积 24-bit
  if state.h == 0:                  // 满 4 符号 → 3 字节
    out[d++] = (a>>16)&0xFF; out[d++] = (a>>8)&0xFF; out[d++] = a&0xFF
flush 处理: switch(state.h){ 1: c(); 2: b(15); a>>=4; emit 1B; 3: b(3); a>>=2; emit 2B }
```

- `l.b([B)[B` = 编码入口:状态机跑两遍(处理边界)→ 输出字节。
- `l.a(String)[B` = `m.a(str)`(UTF-8 bytes)→ `l.b(bytes)` —— **即 keyMaterial 是"UTF-8 后再过自定义 Base64"**。

**重写互通结论**:secretKey 解码 = `Base64.getDecoder().decode(secretKey)`(标准字母表)或 `getUrlDecoder()`(URL-safe)。`java.util.Base64` 直接可用。

## 7. 库内字符串加密(与应用同模板)

`dasho/e.lastIndexOf` / `dasho/c.getChars` 与 stringencrypt 输出的解密器同构:

```
mask = 95                              // ((4+1)-1)^32 算术混淆折叠结果
plain[i] = ct[i] ^ ((key + i) & mask)  // key 逐字符 +1
opaque 源: lastIndexOf()/getChars() → new Random().nextInt(75)+1
调用点: 常量算术压外层密钥 + (r*N)%r≡0 恒定谓词(ifne→fallthrough 真支)+ 双分支 ldc(死支带伪内层)
```

`dasho/c.onsite(String)` 为变体(71 指令),参数顺序/掩码微调,解密同一套表。

调试监听 `dasho/a.a(Ldasho/d;String;[B)V` 把 header/hash 以 `String.format` 拼给监听器——仅诊断用,干净重写可整段删除。

## 8. 干净重写(算法级,非字节码复刻)

以下为独立可读实现,能解 DashO 12.8 加密资源,不依赖 `dasho.*` 包;secretKey 解码即标准 Base64(§6 已确认 j=RFC4648)。

```java
package research.dasho.clean;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import java.io.*;
import java.util.Arrays;
import java.util.Base64;

/**
 * DashO 12.8 资源解密流(算法重写)。
 * 头格式: [4B scrambled-checksum][varint..0x20][16B IV][AES-CBC 密文]
 * 密钥:  scheme+secretKey 来自 Wrapper$Secrets(构建注入);
 *        secretKey 是标准(或 URL-safe) Base64 文本(见 §6)。
 */
public final class DashoResourceDecryptor {

    private static final int BLOCK = 16;           // AES-128
    private static final int HEADER_SCAN_MAX = 17; // varint 最多扫 16+1 字节

    private DashoResourceDecryptor() {}

    /** 门面:等价 Wrapper.decryptWithIOException */
    public static InputStream open(InputStream in, String scheme, String secretKey)
            throws IOException {
        try {
            Cipher cipher = Cipher.getInstance(scheme);
            byte[] key = decodeSecretKey(secretKey);   // 标准 Base64
            String algo = scheme.substring(0, scheme.indexOf('/')); // "AES"
            SecretKeySpec sks = new SecretKeySpec(key, algo);

            Header hdr = parseHeader(in);
            if (hdr == null) {
                // 非加密流:回退为原样(与原库 null 路径一致,这里包一层透传)
                return in;
            }
            // 跳过 varint 字段 + IV
            long skip = (long) hdr.ivOffset + BLOCK;
            long skipped = in.skip(skip);
            if (skipped != skip) throw new IOException("short header skip");

            IvParameterSpec iv = new IvParameterSpec(hdr.iv, 0, BLOCK);
            cipher.init(Cipher.DECRYPT_MODE, sks, iv);
            return new DecryptedStream(in, cipher);
        } catch (IOException | RuntimeException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("dasho resource decrypt init failed", e);
        }
    }

    // ---- 头解析 ----

    static final class Header {
        final byte[] raw;     // 头字节(含 checksum/varint/IV)
        final int ivOffset;   // IV 在 raw 中的起始偏移(= 4 + varintLen)
        final byte[] iv;      // 长度 BLOCK
        Header(byte[] raw, int ivOffset) {
            this.raw = raw;
            this.ivOffset = ivOffset;
            this.iv = Arrays.copyOfRange(raw, ivOffset, ivOffset + BLOCK);
        }
    }

    /** 返回 null = 校验失败(非加密或损坏),调用方回退透传。 */
    static Header parseHeader(InputStream in) throws IOException {
        in.mark(HEADER_SCAN_MAX * 2);
        byte[] buf = new byte[37]; // 原库 mark(74)/read ≤37
        int got = in.read(buf);
        in.reset();
        if (got < 4 + HEADER_SCAN_MAX) return null; // 不够头

        int varintLen = scanVarint(buf, 4);
        if (varintLen < 0) return null;
        int stored = readScrambledU32(buf, 0);
        int actual = polyHash(buf, 4, varintLen, buf, 4 + varintLen, BLOCK);
        if (stored != actual) return null;
        return new Header(buf, 4 + varintLen);
    }

    /** 从 off 起扫到某字节 bit5(0x20) 置位,返回占用字节数;超限返回 -1。 */
    private static int scanVarint(byte[] b, int off) {
        int end = Math.min(b.length, off + HEADER_SCAN_MAX);
        for (int i = off; i < end; i++) {
            if ((b[i] & 0x20) != 0) return i - off + 1;
        }
        return -1;
    }

    /** 字节序打乱的 32 位读:逻辑序 [1,3,0,2]。 */
    static int readScrambledU32(byte[] b, int off) {
        return ((b[off + 1] << 24) & 0xFF000000)
             | ((b[off + 3] << 16) & 0x00FF0000)
             | ((b[off]     <<  8) & 0x0000FF00)
             | ( b[off + 2]        & 0x000000FF);
    }

    /** 多项式哈希 h=h*31+byte,seed=1776651266。 */
    static int polyHash(byte[] a, int aOff, int aLen, byte[] b, int bOff, int bLen) {
        int h = 1776651266;
        for (int i = aOff; i < aOff + aLen; i++) h = h * 31 + a[i];
        for (int i = bOff; i < bOff + bLen; i++) h = h * 31 + b[i];
        return h;
    }

    // ---- 密钥解码(标准 Base64,见 §6) ----

    /**
     * secretKey 是标准 Base64 文本(§6: j=RFC4648 字母表, l 同时接受 +-/_)。
     * 直接 Base64.getDecoder() 即可;若遇 URL-safe 字母表(-_)则 fallback。
     */
    static byte[] decodeSecretKey(String secretKey) {
        try {
            return Base64.getDecoder().decode(secretKey);
        } catch (IllegalArgumentException e) {
            return Base64.getUrlDecoder().decode(secretKey); // fallback: -_
        }
    }

    // ---- 解密流 ----

    static final class DecryptedStream extends InputStream {
        private final InputStream in;
        private final Cipher cipher;
        private final byte[] inBuf = new byte[BLOCK * 256];
        private final byte[] outBuf = new byte[BLOCK * 257];
        private int outPos, outLen;
        private boolean eof;

        DecryptedStream(InputStream in, Cipher cipher) {
            this.in = in; this.cipher = cipher;
        }

        @Override public int read() throws IOException {
            byte[] one = new byte[1];
            int n = read(one, 0, 1);
            return n < 0 ? -1 : one[0] & 0xFF;
        }

        @Override public synchronized int read(byte[] b, int off, int len) throws IOException {
            if (len == 0) return 0;
            if (outPos >= outLen && !refill()) return -1;
            int n = Math.min(len, outLen - outPos);
            System.arraycopy(outBuf, outPos, b, off, n);
            outPos += n;
            return n;
        }

        /** 填一块:读密文→update→写 outBuf。返回是否有新数据。 */
        private boolean refill() throws IOException {
            if (eof) return false;
            int got = in.read(inBuf);
            if (got <= 0) { eof = true; return false; }
            // 原库按块对齐补零;此处对齐到 BLOCK
            int padded = ((got + BLOCK - 1) / BLOCK) * BLOCK;
            if (padded > got) Arrays.fill(inBuf, got, padded, (byte) 0);
            outLen = cipher.update(inBuf, 0, padded, outBuf, 0);
            outPos = 0;
            if (outLen == 0) { eof = true; return false; }
            return true;
        }

        @Override public void close() throws IOException { in.close(); }
    }
}
```

### 重写与原库差异清单

| 项 | 原库 | 重写 |
|----|------|------|
| 调试监听 dasho/d | 全程回显 scheme/key/hash | 删除 |
| 错误消息 | 加密中文串+DecryptionException | 明文 IOException |
| 密钥 Base64 | 自定义 j/k/l 双字母表引擎 | **标准 Base64**(§6 确认 j=RFC4648) |
| mark/reset | mark(74)/reset 全流 | 仅头窗口 mark(2×17) |
| 缓冲策略 | 256B/257B×block 双缓冲+滑动 | 简化单块 update |
| varint 载荷 | 扫描计数,值未用 | 同(互通必需) |
| dasho/k 引擎 | 6-bit 状态机+双字母表+转义 | 直接 `Base64.getDecoder()`,无需复刻 |
