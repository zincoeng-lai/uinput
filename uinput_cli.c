/**
 * @file uinput_cli.c
 * @brief 触摸屏事件注入工具（单点 + 多点 MT 支持）
 *
 * 支持：
 *  - 单点：tap / press / release / longpress / swipe
 *  - 多点：mt-down / mt-move / mt-up（slot-based）
 *  - 坐标映射 (--map W H)
 *  - 独占设备 (--grab)
 *
 * 示例：
 *   @code
 *   uinput -d /dev/input/event1 --map 800 480 tap 400 240
 *   uinput -d /dev/input/event1 --grab mt-down 0 200 120
 *   uinput -d /dev/input/event1 --grab mt-move 0 220 130
 *   uinput -d /dev/input/event1 --grab mt-up 0
 *   @endcode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#define VERSION "1.0"

#define RS_OK 	0
#define RS_ERR -1

/* ------------------ 用户定义 ------------------ */

#define dev_path	"/dev/input/event1"		/* 默认输入设备路径 */
#define MAX_SLOTS	1		/* 最大支持多点触控数量 */
#define SCREEN_W	1920	/* 屏幕宽度 */
#define SCREEN_H	440		/* 屏幕高度 */
#define LOGICAL_W	0		/* 逻辑宽度 */
#define LOGICAL_H	0		/* 逻辑高度 */

/* ------------------ 全局状态 ------------------ */

static const char *def_dev = dev_path;	/* 输入设备路径 */
#if MAX_SLOTS > 1
static int slot_tracking[MAX_SLOTS];	/* slot 对应的 tracking ID */
static int next_tracking_id = 1;	/* 下一个 tracking ID */
static int active_touches = 0;		/* 当前按下的触点数量 */
#endif
/* UI 屏幕逻辑分辨率 */
static int ui_w = LOGICAL_W;
static int ui_h = LOGICAL_H;

/* ------------------ 工具函数 ------------------ */

/**
 * @brief 打开输入设备
 * @param path 输入设备路径（如 /dev/input/eventX）
 * @return 成功返回设备文件描述符，失败返回 RS_ERR
 */
static int open_dev(const char *path) {
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
    return fd >= 0 ? fd : RS_ERR;
}

/**
 * @brief 向输入设备写入一个事件
 * @param fd 设备文件描述符
 * @param type 事件类型
 * @param code 事件代码
 * @param value 事件值
 * @return RS_OK 成功，RS_ERR 失败
 */
static int write_event(int fd, __u16 type, __u16 code, int value) {
    struct input_event ev;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ev.time = tv;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
        fprintf(stderr, "write_event failed: %s\n", strerror(errno));
        return RS_ERR;
    }
    return RS_OK;
}

/**
 * @brief 发送 SYN_REPORT 事件
 * @param fd 设备文件描述符
 * @return RS_OK 成功，RS_ERR 失败
 */
static int syn(int fd) {
	return write_event(fd, EV_SYN, SYN_REPORT, 0);
}

/**
 * @brief 发送 SYN_MT_REPORT 事件
 * @param fd 设备文件描述符
 * @return RS_OK 成功，RS_ERR 失败
 */
static int syn_mt(int fd) {
	return write_event(fd, EV_SYN, SYN_MT_REPORT, 0);
}

/**
 * @brief 毫秒延迟
 */
static void msleep(int ms) {
	if(ms > 0) usleep(ms*1000);
}

/**
 * @brief 读取绝对轴信息
 * @param fd 设备文件描述符
 * @param code 轴代码
 * @param ainfo 存储读取结果的结构体指针
 * @return RS_OK 成功，RS_ERR 失败
 */
static int read_absinfo(int fd, unsigned int code, struct input_absinfo *ainfo) {
    return ioctl(fd, EVIOCGABS(code), ainfo) < 0 ? RS_ERR : RS_OK;
}

/**
 * @brief 映射 UI 坐标到设备坐标（X 轴）
 * @param fd 设备文件描述符
 * @param lx UI 屏幕 X 坐标
 * @return 设备坐标 X
 */
static int map_x(int fd, int lx) {
    if (!ui_w) return lx;
    struct input_absinfo ax;
    if (read_absinfo(fd, ABS_MT_POSITION_X, &ax) < 0 &&
		read_absinfo(fd, ABS_X, &ax) < 0) {
		return lx;
	}
    long long min = ax.minimum, max = ax.maximum;
    return (lx*(max-min)/ui_w + min);
}

/**
 * @brief 映射 UI 坐标到设备坐标（Y 轴）
 * @param fd 设备文件描述符
 * @param ly UI 屏幕 Y 坐标
 * @return 设备坐标 Y
 */
static int map_y(int fd, int ly) {
    if (!ui_h) return ly;
    struct input_absinfo ay;
    if (read_absinfo(fd, ABS_MT_POSITION_Y, &ay) < 0 && read_absinfo(fd, ABS_Y, &ay) < 0) {
		return ly;
	}
    long long min = ay.minimum, max = ay.maximum;
    return (ly*(max-min)/ui_h + min);
}

/**
 * @brief 设置 BTN_TOUCH 状态
 * @param fd 设备文件描述符
 * @param val 状态值（1 按下，0 抬起）
 * @return RS_OK 成功，RS_ERR 失败
 */
static int btn_touch_set(int fd, int val) {
    if (write_event(fd, EV_KEY, BTN_TOUCH, val) == RS_ERR) {
		return RS_ERR;
	}
    return syn(fd);
}

/* ------------------ 单点操作 ------------------ */

/**
 * @brief 单点点击
 * @param fd 设备文件描述符
 * @param lx UI 屏幕 X 坐标
 * @param ly UI 屏幕 Y 坐标
 * @param hold_ms 按下保持时间（毫秒）
 * @return RS_OK 成功，RS_ERR 失败
 */
static int do_tap(int fd, int lx, int ly, int hold_ms) {
    int x = map_x(fd, lx);
	int y = map_y(fd, ly);
	if (write_event(fd, EV_KEY, BTN_TOUCH, 1) == RS_ERR ||
		write_event(fd, EV_KEY, BTN_TOOL_FINGER, 1) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_X, x) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}

    msleep(hold_ms);

    if (write_event(fd, EV_KEY, BTN_TOUCH, 0) == RS_ERR ||
		write_event(fd, EV_KEY, BTN_TOOL_FINGER, 0) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
        syn(fd) == RS_ERR) {
        return RS_ERR;
    }
    return RS_OK;
}

/**
 * @brief 按下操作（不抬起）
 * @param fd 设备文件描述符
 * @param lx UI 屏幕 X 坐标
 * @param ly UI 屏幕 Y 坐标
 * @return RS_OK 成功，RS_ERR 失败
 */
static int do_press(int fd, int lx, int ly) {
    int x = map_x(fd, lx);
	int y = map_y(fd, ly);
	if (write_event(fd, EV_KEY, BTN_TOUCH, 1) == RS_ERR ||
		write_event(fd, EV_KEY, BTN_TOOL_FINGER, 1) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_X, x) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}
	return RS_OK;
}

/**
 * @brief 抬起操作
 * @param fd 设备文件描述符
 * @return RS_OK 成功，RS_ERR 失败
 */
static int do_release(int fd) {
    if (write_event(fd, EV_KEY, BTN_TOUCH, 0) == RS_ERR ||
		write_event(fd, EV_KEY, BTN_TOOL_FINGER, 0) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
        syn(fd) == RS_ERR) {
        return RS_ERR;
    }
    return RS_OK;
}

/**
 * @brief 长按
 * @param fd 设备文件描述符
 * @param lx UI 屏幕 X 坐标
 * @param ly UI 屏幕 Y 坐标
 * @param hold_ms 按下保持时间（毫秒）
 * @return RS_OK 成功，RS_ERR 失败
 */
static int do_longpress(int fd, int lx, int ly, int hold_ms) {
    return do_tap(fd, lx, ly, hold_ms);
}

/**
 * @brief 滑动操作
 * @param fd 设备文件描述符
 * @param x1 起点 X 坐标
 * @param y1 起点 Y 坐标
 * @param x2 终点 X 坐标
 * @param y2 终点 Y 坐标
 * @param duration_ms 滑动总时长（毫秒）
 * @param steps 滑动步数
 * @return RS_OK 成功，RS_ERR 失败
 */
static int do_swipe(int fd,int x1,int y1,int x2,int y2,int duration_ms,int steps) {
    if(steps < 1) {
		steps = 10;
	}

    if(do_tap(fd, x1, y1, 0) == RS_ERR) {
		return RS_ERR;
	}

    for(int i = 1; i <= steps; i++){
        float t = (float)i / steps;
        int xi = x1 + (x2 - x1) * t;
        int yi = y1 + (y2 - y1) * t;
		if (write_event(fd, EV_ABS, ABS_X, map_x(fd, xi)) == RS_ERR ||
			write_event(fd, EV_ABS, ABS_Y, map_y(fd, yi)) == RS_ERR ||
			syn(fd) == RS_ERR) {
			return RS_ERR;
		}
        msleep(duration_ms / steps);
    }

	if (write_event(fd, EV_KEY, BTN_TOUCH, 0) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}

    return RS_OK;
}

#if MAX_SLOTS > 1
/* ------------------ 多点 MT 操作 ------------------ */

/**
 * @brief 多点触控按下
 * @param fd 设备文件描述符
 * @param slot 触控点 slot 编号
 * @param lx UI 屏幕 X 坐标
 * @param ly UI 屏幕 Y 坐标
 * @return RS_OK 成功，RS_ERR 失败
 */
static int mt_down(int fd, int slot, int lx, int ly) {
    if (slot < 0 || slot >= MAX_SLOTS) 
		return RS_ERR;

    int x = map_x(fd, lx);
	int y = map_y(fd, ly);
    int tid = next_tracking_id++;
    if (next_tracking_id > 1000000) {
		next_tracking_id = 1;
	}

    slot_tracking[slot] = tid;
	if (active_touches == 0) {
		if (btn_touch_set(fd, 1) == RS_ERR) {
			return RS_ERR;
		}
	}
    active_touches++;

	if (write_event(fd, EV_ABS, ABS_MT_SLOT, slot) == RS_ERR ||	
		write_event(fd, EV_ABS, ABS_MT_TRACKING_ID, tid) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_X, x) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}

    return RS_OK;
}

/**
 * @brief 多点触控移动
 * @param fd 设备文件描述符
 * @param slot 触控点 slot 编号
 * @param lx UI 屏幕 X 坐标
 * @param ly UI 屏幕 Y 坐标
 * @return RS_OK 成功，RS_ERR 失败
 */
static int mt_move(int fd, int slot, int lx, int ly) {
    if (slot < 0 || slot >= MAX_SLOTS || slot_tracking[slot] == 0) 
		return RS_ERR;

    int x = map_x(fd, lx);
	int y = map_y(fd, ly);

	if (write_event(fd, EV_ABS, ABS_MT_SLOT, slot) == RS_ERR ||	
		write_event(fd, EV_ABS, ABS_MT_POSITION_X, x) == RS_ERR ||
		write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}

    return RS_OK;
}

/**
 * @brief 多点触控抬起
 * @param fd 设备文件描述符
 * @param slot 触控点 slot 编号
 * @return RS_OK 成功，RS_ERR 失败
 */
static int mt_up(int fd, int slot) {
	if (slot < 0 || slot >= MAX_SLOTS || slot_tracking[slot] == 0 ||
		write_event(fd, EV_ABS, ABS_MT_SLOT, slot) == RS_ERR ||	
		write_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1) == RS_ERR ||
		syn_mt(fd) == RS_ERR ||
		syn(fd) == RS_ERR) {
		return RS_ERR;
	}

    slot_tracking[slot] = 0;
    if (--active_touches <= 0) {
        active_touches = 0;
        if (btn_touch_set(fd, 0) == RS_ERR) 
			return RS_ERR;
    }

    return RS_OK;
}
#endif

/* ------------------ 主函数 ------------------ */

/**
 * @brief 打印命令行使用方法
 * @param p 程序名
 * @return 无
 */
static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [-d device] [--grab] [--map W H] <command> ...\n"
        "Commands:\n"
        "  tap X Y [hold_ms]\n"
        "  press X Y\n"
        "  release\n"
        "  longpress X Y hold_ms\n"
        "  swipe X1 Y1 X2 Y2 duration_ms [steps]\n"
        "  mt-down SLOT X Y\n"
        "  mt-move SLOT X Y\n"
        "  mt-up SLOT\n"
        "Examples:\n"
        "  %s -d /dev/input/event1 --map 800 480 tap 400 240\n"
        "  %s -d /dev/input/event1 --grab swipe 100 200 700 200 400 20\n"
        "  %s -d /dev/input/event1 --map 800 480 mt-down 0 200 120\n",
        p, p, p, p);
}

/**
 * @brief 主函数
 * @param argc 参数数量
 * @param argv 参数列表
 * @return RS_OK 成功，RS_ERR 失败
 */
int main(int argc, char **argv) {
	const char *dev = def_dev;
    int grab_flag = 0;
    int opt;
    struct option longopts[] = {
		{"device", 1, NULL, 'd'},
		{"grab", 0, NULL, 'g'},
		{"map", 1, NULL, 0},
		{"help", 0, NULL, 'h'},
		{"version", 0, NULL, 'v'},
		{0, 0, 0, 0}
	};
	
    while((opt = getopt_long(argc, argv, "d:ghv", longopts, NULL)) != -1) {
        if(opt == 'd') {
			dev = optarg;
		}
        else if(opt == 'g') {
			grab_flag = 1;
		}
		else if(opt == 'h') {
			usage(argv[0]);
			return RS_OK;
		}
		else if(opt == 'v') {
			printf("uinput version %s\n", VERSION);
			return RS_OK;
		}
        else if(opt == 0){ 
			ui_w = atoi(optarg);
			ui_h = atoi(argv[optind]);
			optind++;
		}
		else {
		}
    }

    if(optind >= argc){ 
		fprintf(stderr,
				"%s: missing command operand\n"
				"Try '%s' with '--help' or '-h' for more information.\n",
				argv[0], argv[0]);
		return RS_ERR; 
	}

    int fd = open_dev(dev);
    if(fd == RS_ERR) {
		return RS_ERR;
	}

    if(grab_flag) {
		ioctl(fd, EVIOCGRAB, 1);
	}

    const char *cmd = argv[optind];
    int ret = RS_OK;

    if(strcmp(cmd, "tap") == 0) {
		/* 单点触控 */
        int x = atoi(argv[optind+1]);
		int y = atoi(argv[optind+2]);
        int hold = optind+3 < argc ? atoi(argv[optind+3]) : 100;
        ret = do_tap(fd, x, y, hold);
    }
	else if(strcmp(cmd, "press") == 0) {
		/* 按下 */
		int x = atoi(argv[optind+1]);
		int y = atoi(argv[optind+2]);
		ret = do_press(fd, x, y);
	}
	else if(strcmp(cmd, "release") == 0) {
		/* 抬起 */
		ret = do_release(fd);
	}
	else if(strcmp(cmd, "longpress") == 0) {
		/* 长按 */
        int x = atoi(argv[optind+1]);
		int y = atoi(argv[optind+2]);
		int ms = atoi(argv[optind+3]);
        ret = do_longpress(fd, x, y, ms);
    } 
	else if(strcmp(cmd, "swipe") == 0) {
		/* 滑动 */
        int x1 = atoi(argv[optind+1]);
		int y1 = atoi(argv[optind+2]);
        int x2 = atoi(argv[optind+3]);
		int y2 = atoi(argv[optind+4]);
        int ms = atoi(argv[optind+5]);
        int steps = optind+6 < argc ? atoi(argv[optind+6]) : 20;
        ret = do_swipe(fd, x1, y1, x2, y2, ms, steps);
    } 
#if MAX_SLOTS > 1
	else if(strcmp(cmd, "mt-down") == 0) {
		/* 多点触控 */
        int slot = atoi(argv[optind+1]);
        int x = atoi(argv[optind+2]);
		int y = atoi(argv[optind+3]);
        ret = mt_down(fd, slot, x, y);
    } 
	else if(strcmp(cmd, "mt-move") == 0) {
		/* 多点触控移动 */
        int slot = atoi(argv[optind+1]);
        int x = atoi(argv[optind+2]);
		int y = atoi(argv[optind+3]);
        ret = mt_move(fd, slot, x, y);
    } 
	else if(strcmp(cmd, "mt-up") == 0) {
		/* 多点触控抬起 */
        int slot = atoi(argv[optind+1]);
        ret = mt_up(fd, slot);
    } 
#endif
	else {
		/* 其他命令 */
		usage(argv[0]);
	}
    close(fd);
    return ret;
}
