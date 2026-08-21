# 编译命令
## 使用MSYS2
```bash
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o <输出文件>.exe <源文件>.c  `pkg-config --libs gtk4`
```
## 关闭控制台
```bash
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o <输出文件>.exe <源文件>.c `pkg-config --libs gtk4` -mwindows
```
## 关闭控制台并添加图标
### 创建一个文本文件，命名为 *.rc，内容如下：
```text
1 ICON "app.ico"
```

```bash
windres <文件名>.rc -o <文件名>.o 
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o <输出文件>.exe <源文件>.c <文件名>.o `pkg-config --libs gtk4` -mwindows
```
