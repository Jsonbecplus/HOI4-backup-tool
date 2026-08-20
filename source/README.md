#编译命令
使用MSYS2
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o <输出文件>.exe <源文件>.c debug_res.o `pkg-config --libs gtk4`
关闭控制台
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o hoi_backup_tool.exe hoi_backup_tool.c `pkg-config --libs gtk4` -mwindows
关闭控制台并添加图标
gcc -Wall -Wextra -O2 `pkg-config --cflags gtk4` -o hoi_backup_tool.exe hoi_backup_tool.c ico_res.o `pkg-config --libs gtk4` -mwindows
