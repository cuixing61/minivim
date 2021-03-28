/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h> //ioctl(), TIOCGWINSZ, struct winsize
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define STACK_INIT_SIZE 20

//定义ctrl组合输入的宏函数
#define CTRL_KEY(k) ((k)&0x1f)
/*建立特殊按键与数值的映射
将所有字符转化为数值，方便判断*/
enum editorKey
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

#define RECORD_TIMES 20
/*** data ***/
//行信息
typedef struct erow
{
    int size;
    int rsize; //实际大小（包括了不显示的字符）
    char *chars;
    char *render; //实际渲染
} erow;
//全局变量，编辑器参数
typedef struct editorConfig
{
    int cx, cy;     //光标坐标
    int rx;         //制表符行坐标，没有制表符时一切与cx相同
    int rowoff;     //行偏移
    int coloff;     //列偏移
    int screenrows; //窗口的行数
    int screencols; //窗口的列数
    int numrows;    //行总数
    erow *row;
    int dirty; //文件修改程度，保存文件就置为0
    char *filename;
    char statusmsg[80];    //状态信息
    time_t statusmsg_time; //状态信息显示时间长度
    struct termios orig_termios;
} SElemType;

struct editorConfig E;
int tmp = 0;
/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
//char *editorPrompt(char *prompt); change!
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

//它将打印错误消息并退出程序。
void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    //打印错误信息
    perror(s);

    //退出程序
    exit(1);
}
//回到原始模式
void disableRawMode()
{ //报错
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}
//开启raw模式
void enableRawMode()
{ //检测原始终端模式
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    //程序退出时调用disableRawMode，回到原始模式
    atexit(disableRawMode);
    //将原始终端模式赋值给raw，对raw进行修改
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST); //关闭输出
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); //关闭回显，经典模式
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    //将修改好的raw模式传入终端属性
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        //报错
        die("tcsetattr");
}

//按键读取，处理转义序列，返回按键的处理后的输入
int editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    { //检测读取是否成功
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}
/*获得光标坐标，用指针传值
通过'\x1b[6n',将光标坐标写入标准输出，再读取R结尾前的坐标*/

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;
    //将光标坐标写入标准输出
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    //将光标坐标写入buf
    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    //提取光标坐标
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

//获取窗口大小,因为传入的行数，和列数的指针，所以可以传递行数和列数的值
int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    //-1为ioctl出错，ws_col=0显然也是个错误
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    { /*b方案：ioctl()不能保证能够请求所有系统上的窗口大小，因此我们将提供一种获取窗口大小的回退方法。
   策略是用超大值将光标定位在屏幕的右下角，然后使用转义序列来查询光标的位置。它告诉我们屏幕上有多少行和列。*/

        //如果这种方法也不行，就报错。
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        //这种方法同样可以用指针返回行数和列数。
        return getCursorPosition(rows, cols);
    }
    else
    { //a方案可以：
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/
//将cx转换为rx
int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    { //强制对齐到制表符结尾
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

//将rx转换为cx
int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx)
            return cx;
    }
    return cx;
}

//将每行的文本转化为实际渲染（处理制表符）
void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;
    //找出有多少个制表符
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;
    //分配每行的实际大小
    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        //在指标符处，向实际行字符串中添加空格，直到8的倍数
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

//插入行
void editorInsertRow(int at, char *s, size_t len)
{ //检测坐标在文本内
    if (at < 0 || at > E.numrows)
        return;
    //重新分配行信息数组
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    //重建第at行
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    //更新第at行的实际渲染
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
}

//删除一行
void editorDelRow(int at)
{ //确定不超出文件范围
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    //使用memmove()来用后面的行覆盖被删除的行
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

//在行中插入字符
void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;
    //重新分配更大的空间
    row->chars = realloc(row->chars, row->size + 2);
    //将at的内容转移到at+1，空出at
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    //在at处插入字符
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

//在行尾添加字符字符串
void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}
//在行中删除字符
void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;
    //使用memmove()来用后面的字符覆盖被删除的字符
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/
//接受一个字符并使用editorRowInsertChar()将该字符插入到光标所在的位置。

void editorInsertChar(int c)
{
    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}
//处理新建行
void editorInsertNewline()
{
    //在行头
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    //在行中按回车
    else
    {
        erow *row = &E.row[E.cy];
        //新行的内容
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        //截断旧行
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}
//确定删除字符的位置
void editorDelChar()
{
    if (E.cy == E.numrows)
        return;
    //游标位于开头
    if (E.cx == 0 && E.cy == 0)
        return;
    //确定删除哪一行
    erow *row = &E.row[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    //在行头删除
    else
    {
        E.cx = E.row[E.cy - 1].size;
        //在行尾添加字符字符串
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/******************redo&undo******************************/
/******************redo&undo******************************/

void initSElemType(SElemType **e2) //初始化临时窗口状态
{
    *e2 = (SElemType *)malloc(sizeof(SElemType));
}

void CopySElemType(SElemType *e2, SElemType *e1)
{
    // e2=(SElemType*)malloc(sizeof(SElemType));
    e2->cx = e1->cx;
    e2->cy = e1->cy;
    e2->rx = e1->rx;
    e2->rowoff = e1->rowoff;
    e2->coloff = e1->coloff;
    e2->numrows = e1->numrows;

    e2->row = (erow *)malloc(e1->numrows * sizeof(erow));
    int i;
    for (i = 0; i < e1->numrows; i++)
    {
        e2->row[i].size = e1->row[i].size;
        e2->row[i].rsize = e1->row[i].rsize;
        e2->row[i].chars = (char *)malloc(e1->row[i].size * sizeof(char));
        strcpy(e2->row[i].chars, e1->row[i].chars);
        e2->row[i].render = (char *)malloc(e1->row[i].size * sizeof(char));
        strcpy(e2->row[i].render, e1->row[i].render);
    }
    e2->dirty = e1->dirty;
    e2->filename = (char *)malloc(sizeof(char) * 10);
    strcpy(e2->filename, e1->filename);
    e2->screenrows = e1->screenrows;
    e2->screencols = e1->screencols;
    //e2->statusmsg = (char*)malloc(80*sizeof(char));
    strcpy(e2->statusmsg, e1->statusmsg);
    e2->statusmsg_time = e1->statusmsg_time;
    e2->orig_termios = e1->orig_termios;
}

typedef struct //保存窗口状态的结构体
{
    SElemType *base[STACK_INIT_SIZE]; //指针数组
    int rear;
    int current;
    int flag;
} SqStack;

SqStack S;

int InitStack(SqStack *S) //初始化结构体
{
    int i;
    for (i = 0; i < STACK_INIT_SIZE; i++)
        S->base[i] = NULL;
    S->current = 0;
    S->rear = 0;
    S->flag = 0;
    return 0;
} //InitStack

void myfree(SElemType *E) //释放空间
{

    for (int i = 0; i < E->numrows; i++)
    {
        free(E->row[i].chars);
        free(E->row[i].render);
    }
    free(E->row);
    free(E->filename);
    //free(E->statusmsg);
    free(E);
}

int AddRecord(SqStack *S) //调用的时候AddRecord(S,&e1),传值，传地址
{
    //首先要拷贝当前e的状态
    SElemType *e2;
    initSElemType(&e2);
    CopySElemType(e2, &E);
    int i;
    if (S->rear == STACK_INIT_SIZE)
    {
        if (tmp == 1)
        {
            myfree(S->base[0]);
        }
        for (i = 0; i < STACK_INIT_SIZE - 1; i++)
        {
            S->base[i] = S->base[i + 1];
        }
        S->base[S->rear - 1] = e2;
    }
    else
    {
        S->base[S->rear] = e2;
        S->rear = S->rear + 1;
    }

    S->current = S->rear;

    return 0;
} //Push入栈操作

void editorUndo()
{
    if (S.current <= 0)
    {
        S.current = S.current + STACK_INIT_SIZE;
    }
    if (S.base[S.current - 1] != NULL && S.flag < STACK_INIT_SIZE)
    {
        E = *S.base[S.current - 1];
        S.current--;
        S.flag++;
    }

    editorSetStatusMessage("undo");
}

void editorRedo()
{

    if (S.flag > 1)
    {
        E = *S.base[(S.current + 1) % STACK_INIT_SIZE];
        S.current = (S.current + 1) % STACK_INIT_SIZE;
        S.flag--;
    };

    editorSetStatusMessage("redo");
}
/*** file i/o ***/
//将文件内容写入缓冲区
char *editorRowsToString(int *buflen)
{
    //得到文件总长
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;
    //分配文件总大小
    char *buf = malloc(totlen);
    char *p = buf;
    //文件内容写入缓冲区，每行结尾添加回车
    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{ //保存路径
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    //getline：对line指针与行的长度赋值
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    { //去掉行位的换行
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename == NULL)
    { //change
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }
    //文件总长
    int len;
    //文件内容缓冲区
    char *buf = editorRowsToString(&len);
    //如果文件不存在，我们希望创建一个新文件(O_CREAT)，并希望打开它进行读写(O_RDWR)
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    //处理错误
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/
int *KMP(char *s, char *p)
{
    int len = strlen(p);
    int r = -1;
    int t = 0;
    int next[20];
    next[0] = -1;
    //	printf("\n j= %d next[0]%d",j,next[j]);
    while (t < len)
    {
        if (r == -1 || p[r] == p[t])
        {
            r++;
            t++;
            next[t] = r;
            //			printf("\n j= %d next[%d]%d",j,j,next[j]);
        }
        else
        {
            r = next[r];
        }
    }
    int i = 0;
    int j = 0;
    int sLen = strlen(s);
    int pLen = strlen(p);
    int k = 0;
    int *res;
    res = (int *)malloc(sizeof(int) * 20);
    while (i < sLen && j < pLen)
    {
        if (j == -1 || s[i] == p[j])
        {
            i++;
            j++;
            if (j == pLen)
            {
                res[k] = (i - j);
                k++;
            }
        }
        else
        {
            //②如果j != -1，且当前字符匹配失败（即S[i] != P[j]），则令 i 不变，j = next[j]
            //next[j]即为j所对应的next值
            j = next[j];
        }
    }
    res[k] = -1;
    return res;
}
//定义回调函数用于增量查找，对每一种按键都作相应操作
void editorFindCallback(char *query, int key)
{
    static int last_match = -1; //上一个匹配字符串序号，初始值为-1
    static int direction = 1;   //搜索方向

    //退出时重新初始化值
    if (key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    //方向键控制搜索方向
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1)
        direction = 1;
    int current = last_match;
    int i;
    //行循环搜索
    for (i = 0; i < E.numrows; i++)
    {
        current += direction;
        if (current == -1)
            current = E.numrows - 1;
        else if (current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        //char *match = strstr(row->render, query);
        char *match = NULL;
        if (KMP(row->render, query)[0] != -1)
            match = row->render + KMP(row->render, query)[0];
        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorFind()
{ //保存进入search模式前的光标位置，使退出search时光标重新定位
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    //editorFindCallback作editorPrompt的回调函数（函数作参数）
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
                               editorFindCallback);

    if (query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** replace***/
//定义字符串替换函数
char *replaceWord(char *s, char *oldW, char *newW)
{
    char *result;
    int i, cnt = 0;
    int newWlen = strlen(newW);
    int oldWlen = strlen(oldW);

    for (i = 0; s[i] != '\0'; i++)
    {
        if (strstr(&s[i], oldW) == &s[i])
        {
            cnt++;

            i += oldWlen - 1;
        }
    }

    //保证足够内存
    result = (char *)malloc(i + cnt * (newWlen - oldWlen) + 1);

    i = 0;
    while (*s)
    {
        if (strstr(s, oldW) == s)
        {
            strcpy(&result[i], newW);
            i += newWlen;
            s += oldWlen;
        }
        else
            result[i++] = *s++;
    }

    result[i] = '\0';
    return result;
}

//基础版的接受屏幕输入函数
char *editorPrompt0(char *prompt)
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

//替换函数
void editorReplace()
{
    char *query = editorPrompt0("Search: %s (ESC to cancel)");
    if (query == NULL)
        return;
    //查找
    int i;
    for (i = 0; i < E.numrows; i++)
    {
        erow *row = &E.row[i];
        char *match = strstr(row->render, query);
        if (match)
        {
            E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            char *replace = editorPrompt0("Replace: %s (ESC to cancel)");
            //使指向原子字符串的指针指向替换后的字符串
            row->chars = replaceWord(row->render, query, replace);
            row->render = replaceWord(row->render, query, replace);
            free(replace);
            break;
        }
    }

    free(query);
}

/*** append buffer ***/
//可附加缓冲区
struct abuf
{
    char *b; //追加的内容
    int len; //追加的长度
};

#define ABUF_INIT \
    {             \
        NULL, 0   \
    }
//对可追加缓冲区进行追加
void abAppend(struct abuf *ab, const char *s, int len)
{ //新建一个指向长度变更后的字符串指针，包含原内容
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    //从追加位置开始拷贝
    memcpy(&new[ab->len], s, len);
    //替换
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/
/*我们的策略是检查光标是否移到了可见窗口之外，如果是，调整E.rowoff，使光标正好位于可见窗口内。*/

void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    //当光标小于偏移量时，E.rowoff = E.cy，显示时的坐标为 E.cy-E.rowoff=1

    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    /*当光标超过偏移量+显示范围，就让E.rowoff = E.cy - E.screenrows + 1
  显示时坐标为E.y-E.rowoff=E.screenrows
  */
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}
//按行绘制
void editorDrawRows(struct abuf *ab)
{
    int y;
    //显示小于窗口的行数的内容
    for (y = 0; y < E.screenrows; y++)
    { /*相当于从E.row[rowoff]的位置开始显示
    */
        int filerow = y + E.rowoff;
        //对于大于文件行数的行，行首打印'~'
        if (filerow >= E.numrows)
        {
            //在不加载文件的条件下，打印欢迎信息，首先使欢迎信息行定位

            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "miword -- version %s", KILO_VERSION);
                //窗口不够宽，截断欢迎信息
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                //使欢迎信息列居中
                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
            { //打印文件,从列偏移的位置开始打印每行
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            //截断超过窗口的部分
            if (len > E.screencols)
                len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }
        //清除光标右端至结尾
        abAppend(ab, "\x1b[K", 3);
        //换行，光标置于行首
        abAppend(ab, "\r\n", 2);
    }
}
//绘制状态栏
void editorDrawStatusBar(struct abuf *ab)
{
    //修改颜色
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    //打印路径（新建文件路径为[No Name]）与文件行数
    //如果文件已被修改，则在文件名后显示(已修改)。
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows,
                       E.dirty ? "(modified)" : "");
    //打印光标所在行数与文件总行数
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                        E.cy + 1, E.numrows);
    //窗口不够大，信息截断
    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols)
    { //当第2条信息的尾部与窗口对齐
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        { //在两条信息间打印空格
            abAppend(ab, " ", 1);
            len++;
        }
    }
    //将颜色调回
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

//绘制状态信息
void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

//刷新屏幕
void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;
    //刷新屏幕时隐藏光标
    abAppend(&ab, "\x1b[?25l", 6);
    //重置光标位置
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    //绘制状态栏栏
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    //终端使用1引索，C语言使用0引索，注意行坐标采用的是rx
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    //显示光标
    abAppend(&ab, "\x1b[?25h", 6);

    //我们将缓冲区的内容写入标准输出，并释放abuf使用的内存。
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
//打印信息
void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        //确保输入不是特殊字符
        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        //取消输入
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        { //参数输入空间不够，拓展空间
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback)
            callback(buf, c);
    }
}
//光标移动
void editorMoveCursor(int key)
{ //防止光标的列数大于文本
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        //防止出现负数越界
        if (E.cx != 0)
        {
            E.cx--;
        }
        //从行头切换到上一行的行尾。
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        //防止光标超过该行的文本
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        //行尾按右箭头跳转到下一行行首
        else if (row && E.cx == row->size)
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows)
        {
            E.cy++;
        }
        break;
    }
    //如果如果上一行的长度大于下一行，从较长一行的尾，切换到下一行的行尾

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

//将把各种Ctrl键组合和其他特殊键映射到不同的编辑器函数，并将任何字母数字和其他可打印键的字符插入到正在编辑的文本中。
void editorProcessKeypress()
{
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;
        //根据文件的修改程度与按ctrl-q的次数，在未保存的情况下，需连按3次ctrl-q

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        //退出时，刷新屏幕
        write(STDOUT_FILENO, "\x1b[2J", 4);
        //退出时重置光标位置
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    //保存
    case CTRL_KEY('s'):
        editorSave();
        break;
    //撤销
    case CTRL_KEY('z'):
        editorUndo();
        break;
        //重做
    case CTRL_KEY('y'):
        editorRedo();
        break;
    //回到行头部
    case HOME_KEY:
        E.cx = 0;
        break;
    //去行尾
    case END_KEY:
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    //查找
    case CTRL_KEY('f'):
        editorFind();
        break;

    //替换
    case CTRL_KEY('r'):
        editorReplace();
        break;
    //删除
    case BACKSPACE:
    case CTRL_KEY('h'):

    case DEL_KEY:
        if (c == DEL_KEY)
            //DEL建相当于先按一次右箭头，再删除
            editorMoveCursor(ARROW_RIGHT);
        if (E.filename != NULL)
            AddRecord(&S);

        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    { //回到窗口的顶部
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        //回到窗口的底部
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
        //方向键
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;
        //若按键不特殊，就插入

    default:
        editorInsertChar(c);
        break;
    }
    //若输入非ctrl-q，重置退出警告次数
    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();

    InitStack(&S);
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage( //change!
        "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctro-R = replace");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
