#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "agents/game.h"
#include "agents/mcts.h"
#include "agents/negamax.h"
#include "list.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ABUF_INIT \
    {             \
        NULL, 0   \
    }

struct task {
    jmp_buf env;
    struct list_head list;
    char task_name[10];
    int n;
    int i;
};

struct arg {
    int n;
    int i;
    char *task_name;
};

static struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

/*** append buffer ***/
struct abuf {
    char *b;
    int len;
};

static LIST_HEAD(tasklist);
static void (**tasks)(void *);
static struct arg *args;
static int ntasks;
static jmp_buf sched;
static struct task *cur_task;
static char table[N_GRIDS];
static char draw_buffer[DRAWBUFFER_SIZE];
static char attr = 0;
static char *mem_step;
static unsigned int mem_step_size = 10;
static unsigned int mem_step_counter;
static struct editorConfig E;
static void listen_keyboard_handler_nonmodule(void);
void editorDrawStatusBar();
void editorClearStatusBar();
void editorRefreshStatusBar();

static inline void enlarge_mem_step()
{
    if (mem_step_size - mem_step_counter == 2) {
        mem_step = realloc(mem_step, mem_step_size * 2);
        printf("---------- enlarge success -----\n");
        mem_step_size *= 2;
    }
}

static void print_mem_step()
{
    int counter = 0;
    char temp = mem_step[counter];
    while (counter < mem_step_counter) {
        while (temp) {
            switch (temp % 4) {
            case 0:
                printf("D%d -> ", temp / 4 + 1);
                break;
            case 1:
                printf("A%d -> ", temp / 4 + 1);
                break;
            case 2:
                printf("B%d -> ", temp / 4 + 1);
                break;
            case 3:
                printf("C%d -> ", temp / 4 + 1);
                break;
            default:
                break;
            }
            temp = mem_step[++counter];
        }

        printf("DONE\n");
        temp = mem_step[++counter];
    }
}

static void task_add(struct task *task)
{
    list_add_tail(&task->list, &tasklist);
}

static void task_add_head(struct task *task)
{
    list_add(&task->list, &tasklist);
}

static void task_switch()
{
    if (!list_empty(&tasklist)) {
        struct task *t = list_first_entry(&tasklist, struct task, list);
        list_del(&t->list);
        cur_task = t;
        longjmp(t->env, 1);
    }
}

void schedule(void)
{
    static int i;

    setjmp(sched);

    while (ntasks-- > 0) {
        struct arg arg = args[i];
        tasks[i++](&arg);
        printf("Never reached\n");
    }

    task_switch();
}

/* A task yields control n times */

void task0(void *arg)
{
    struct task *task = malloc(sizeof(struct task));
    strcpy(task->task_name, ((struct arg *) arg)->task_name);
    task->n = ((struct arg *) arg)->n;
    task->i = ((struct arg *) arg)->i;
    INIT_LIST_HEAD(&task->list);

    printf("%s: n = %d\n", task->task_name, task->n);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    for (; task->i < task->n; task->i += 2) {
        if (setjmp(task->env) == 0) {
            // long long res = fib_sequence(task->i);
            // printf("%s fib(%d) = %lld\n", task->task_name, task->i, res);
            char win = check_win(table);
            if (win == ' ') {
                int predict = mcts(table, 'O');
                table[predict] = 'O';
                enlarge_mem_step();
                mem_step[mem_step_counter++] = predict + 1;
                editorRefreshStatusBar();
            } else {
                enlarge_mem_step();
                mem_step[mem_step_counter++] = 0;
                editorRefreshStatusBar();
                memset(table, ' ',
                       N_GRIDS); /* Reset the table so the game restart */
                printf("kxo: %c win!!!\n", win);
            }

            task_add(task);
            task_switch();
        }
        task = cur_task;
        printf("%s: resume\n", task->task_name);
    }

    printf("%s: complete\n", task->task_name);
    longjmp(sched, 1);
}

void task1(void *arg)
{
    struct task *task = malloc(sizeof(struct task));
    strcpy(task->task_name, ((struct arg *) arg)->task_name);
    task->n = ((struct arg *) arg)->n;
    task->i = ((struct arg *) arg)->i;
    INIT_LIST_HEAD(&task->list);

    printf("%s: n = %d\n", task->task_name, task->n);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    for (; task->i < task->n; task->i++) {
        if (setjmp(task->env) == 0) {
            // printf("%s %d\n", task->task_name, task->i);
            char win = check_win(table);
            if (win == ' ') {
                int move = negamax_predict(table, 'X').move;
                table[move] = 'X';
                enlarge_mem_step();
                mem_step[mem_step_counter++] = move + 1;
                editorRefreshStatusBar();
            } else {
                enlarge_mem_step();
                mem_step[mem_step_counter++] = 0;
                editorRefreshStatusBar();
                memset(table, ' ',
                       N_GRIDS); /* Reset the table so the game restart */
                printf("kxo: %c win!!!\n", win);
            }

            task_add(task);
            task_switch();
        }
        task = cur_task;
        printf("%s: resume\n", task->task_name);
    }

    printf("%s: complete\n", task->task_name);
    longjmp(sched, 1);
}

void task2(void *arg)
{
    struct task *task = malloc(sizeof(struct task));
    strcpy(task->task_name, ((struct arg *) arg)->task_name);
    task->n = ((struct arg *) arg)->n;
    task->i = ((struct arg *) arg)->i;
    INIT_LIST_HEAD(&task->list);

    printf("%s: n = %d\n", task->task_name, task->n);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    for (; task->i < task->n; task->i++) {
        if (setjmp(task->env) == 0) {
            listen_keyboard_handler_nonmodule();
            if (attr)
                task_add_head(task);
            else
                task_add(task);
            task_switch();
        }
        task = cur_task;
        printf("%s: resume\n", task->task_name);
    }

    printf("%s: complete\n", task->task_name);
    longjmp(sched, 1);
}

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &E.orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = E.orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_iflag &= ~(IXON | IXOFF);  // disable ctrl+s ctrl+q
    raw.c_cc[VMIN] = 0;              // read() minimum numnber of byte
    raw.c_cc[VTIME] = 1;             // read() timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editorDrawStatusBar()
{
    write(STDOUT_FILENO, "\x1b[7m", 4);
    time_t current = time(NULL);
    char status[80];
    int len = snprintf(status, sizeof(status), "UTC:       %s",
                       asctime(gmtime(&current)));
    write(STDOUT_FILENO, status, 35);
    write(STDOUT_FILENO, "\x1b[m", 3);
}

/* purpose: clear one line
 * method: move the cursor to left and clear the screan until end
 *
 */
void editorClearStatusBar()
{
    write(STDOUT_FILENO, "\x1b[50D", 5);
    write(STDOUT_FILENO, "\x1b[2K", 4);
}

void editorRefreshStatusBar()
{
    editorClearStatusBar();
    draw_board(table);
    editorDrawStatusBar();
}

static void listen_keyboard_handler_nonmodule(void)
{
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */ /* It should remove the task for work1 & work2 */
            attr ^= 1;
            printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */ /* It should quit the program */
            printf("Stopping the kernel space tic-tac-toe game...\n");
            print_mem_step();
            exit(0);
        }
    }
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

static void draw_bitmap(const unsigned int bitmap)
{
    char table[N_GRIDS];
    for (int i = 0; i < 32; i += 2) {
        if (bitmap & (1 << i)) {
            table[i / 2] = 'O';
        } else if (bitmap & (1 << i + 1)) {
            table[i / 2] = 'X';
        }
    }
    draw_board(table);
}

int main(int argc, char *argv[])
{
    if (argc == 1) {
        if (!status_check())
            exit(1);

        raw_mode_enable();
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

        char display_buf[DRAWBUFFER_SIZE];
        unsigned int read_result;

        fd_set readset;
        int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
        int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
        read_attr = true;
        end_attr = false;

        while (!end_attr) {
            FD_ZERO(&readset);
            FD_SET(STDIN_FILENO, &readset);
            FD_SET(device_fd, &readset);

            int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
            if (result < 0) {
                printf("Error with select system call\n");
                exit(1);
            }

            if (FD_ISSET(STDIN_FILENO, &readset)) {
                FD_CLR(STDIN_FILENO, &readset);
                listen_keyboard_handler();
            } else if (read_attr && FD_ISSET(device_fd, &readset)) {
                FD_CLR(device_fd, &readset);
                printf(
                    "\033[H\033[J"); /* ASCII escape code to clear the screen */
                // read(device_fd, display_buf, DRAWBUFFER_SIZE);
                // printf("%s", display_buf);
                read(device_fd, &read_result, sizeof(read_result));
                draw_bitmap(read_result);
                printf("%x\n", read_result);
            }
        }

        raw_mode_disable();
        fcntl(STDIN_FILENO, F_SETFL, flags);

        close(device_fd);

    } else {
        negamax_init();
        mem_step = malloc(sizeof(char) * mem_step_size);
        memset(table, ' ', N_GRIDS);

        void (*registered_task[])(void *) = {task0, task1, task2};
        struct arg arg0 = {.n = 2000, .i = 0, .task_name = "Task 0"};
        struct arg arg1 = {.n = 2000, .i = 1, .task_name = "Task 1"};
        struct arg arg2 = {.n = 6000, .i = 0, .task_name = "Task 2"};
        struct arg registered_arg[] = {arg0, arg1, arg2};
        tasks = registered_task;
        args = registered_arg;
        ntasks = ARRAY_SIZE(registered_task);
        raw_mode_enable();
        schedule();
        raw_mode_disable();
    }
    return 0;
}
