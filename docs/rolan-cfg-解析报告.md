# Rolan 配置格式解析（用于迁移到本启动器）

没有逆向，纯格式分析。结论：**Rolan 的 `Rolan.cfg` / `.rcb`（备份）没有做真正的加密**，
只是 base64 之前把字母做了大小写互换——所以任何语言的 3 行代码就能读出来。

## 一、文件结构

- INI 风格、**GBK 编码**（中文 Windows 下直接按 GBK 读即可）。
- `[Config]`：窗口位置、标题、上次打开的面板等。
- `[Data]`：每个键 = 一个面板名，值 = 逗号分隔的条目密文。逗号是分隔符，不参与密文，单条密文 68–500 字节不等。

`[Data]` 里的一行长这样（面板名已被换成示例）：

```
常用工具=<逗号分隔的条目密文>
```

## 二、解密算法（关键，别去爆破 XOR/AES）

```python
import base64
p = 密文.swapcase()                                   # 大小写互换
明文 = base64.b64decode(p + '=' * (-len(p) % 4)).decode('gbk')
```

`[Config]` 里的标题同法解：形如 `06A8SC/S06A5Pl7F` 的解出来是「常用工具」这样的面板名。

单条明文格式（和 Rolan 自己的写法一致）：

```
Title:"记事本",Path:"%rp%\系统工具\notepad.exe",IcoPath:"%rp%\系统工具\notepad.exe",Parm:"",Count:"0",
```

- `%rp%` = **Rolan 程序所在目录**，是相对路径写法，天然可移植。
- `Parm` 是启动参数（常为空）；`Count` 是启动次数，迁移时无价值，直接丢掉。

## 三、迁移到本启动器的注意事项

1. **先统计相对路径 vs 绝对路径**：含 `%rp%` 的条目天然可移植；写成 `D:\...` 的绝对路径换机器就可能失效。
2. 绝对路径若落在工具箱根目录（原 `Rolan.exe` 所在目录）之内，剪掉根前缀改成相对路径即可全部可移植；
   落在根目录之外的工具，需要单独决定是"一起拷进工具箱"还是"保留绝对路径"。
3. **新启动器必须放在工具箱根目录**——配置里的路径是相对它解析的。
4. 转换出来的配置写进 `tools_utf8.txt`，一行一个工具：`面板|名称|相对路径|参数`。
   面板名、条目顺序都可以直接用；`Count` 丢掉即可。

## 四、转换成 tools_utf8.txt 的脚本

本仓库不带具体转换脚本（它只对本机路径有意义），思路就是上面三行解密 + 一个 `%rp%` 前缀替换：

```python
lines = []
for panel, items in data.items():          # data 由 [Data] 段逐行解密得到
    for it in items:
        rel = it["Path"].replace("%rp%\\", "").replace("%rp%", "")
        lines.append(f'{panel}|{it["Title"]}|{rel}|{it.get("Parm","")}')
open("tools_utf8.txt", "w", encoding="utf-8", newline="\r\n").write("\r\n".join(lines) + "\r\n")
```

（参考出处：公开博客中关于 Rolan 1.3.6 cfg 的解析讨论，算法一致。）
