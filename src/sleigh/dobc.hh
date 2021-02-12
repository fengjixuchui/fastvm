﻿
#include "mcore/mcore.h"
#include "elfloadimage.hh"

typedef struct funcdata     funcdata;
typedef struct pcodeop      pcodeop;
typedef struct varnode      varnode;
typedef struct flowblock    flowblock, blockbasic, blockgraph;
typedef struct dobc         dobc;
typedef struct jmptable     jmptable;
typedef struct cpuctx       cpuctx;
typedef struct funcproto    funcproto;
typedef struct rangenode    rangenode;
typedef struct func_call_specs  func_call_specs;
typedef map<Address, vector<varnode *> > variable_stack;
typedef map<Address, int> version_map;
typedef struct cover		cover;
typedef struct valuetype    valuetype;
typedef struct coverblock	coverblock;

class pcodeemit2 : public PcodeEmit {
public:
    funcdata *fd = NULL;
    FILE *fp = stdout;
    virtual void dump(const Address &address, OpCode opc, VarnodeData *outvar, VarnodeData *vars, int size);

    void set_fp(FILE *f) { fp = f;  }
};

enum ARMInstType {
    a_null,
    a_stmdb,
    a_sub,
    a_add,
};

struct VisitStat {
    SeqNum seqnum;
    int size;
    struct {
        unsigned condinst: 1;
    } flags;

    enum ARMInstType    inst_type;
};

enum height {
    a_top,
    /* 
    普通常量:
    mov r0, 1
    这样的就是普通的常量, 1是常量, 赋给r0以后，当前的r0也成了常量 */

    a_constant,
    /*
    相对常量

    0x0001. add esp, 4
    0x0002. ...
    0x0003. ...
    0x0004. sub esp, 16
    
    esp就是相对常量，我们在做一些分析的时候，会使用模拟的方式来计算esp，比如给esp一个初始值，这种方式在分析领域不能算错，
    因为比如像IDA或者Ghidra，都有对错误的容忍能力，最终的结果还是需要开发人员自己判断，但是在编译还原领域，不能有这种模糊语义
    的行为。

    假设壳的开发人员，在某个代码中采取了判断esp值的方式来实现了部分功能，比如
    if (esp > xxxxx)  {} else {}
    很可能其中一个 condition_block 就会被判断成 unreachable code。这个是给esp强行赋值带来的结果，但是不赋值可能很多计算进行不下去

    所以我们把 inst.0x0001 中的 esp设置为相对常量(rel_constant)，他的值为 esp.4, 0x0004中的esp 为  esp.16，在做常量传播时，有以下几个规则

    1. const op const = const，      (常量和常量可以互相操作，比如 加减乘除之类)
    2. sp.rel_const op const = sp.rel_const;   (非常危险，要判断这个操作不是在循环内)
    3. sp.rel_const op sp.rel_const = sp.rel_const      (这种情况起始非常少见，)
    4. sp.rel_const op rN.rel_const (top)   (不同地址的相对常量不能参与互相运算)
    */ 
    a_rel_constant,
    a_bottom,

    /* */
};

struct valuetype {
    enum height height = a_top;
    intb v = 0;
    Address rel;

    int cmp(const valuetype &b) const;
    bool operator<(const valuetype &b) const { return cmp(b) < 0; }
    bool operator==(const valuetype &b) { return cmp(b) == 0;  }
    bool operator!=(const valuetype &b) { return !operator==(b); }
};

struct varnode_cmp_loc_def {
    bool operator()(const varnode *a, const varnode *b) const;
};

struct varnode_cmp_def_loc {
    bool operator()(const varnode *a, const varnode *b) const;
};

typedef set<varnode *, varnode_cmp_loc_def> varnode_loc_set;
typedef set<varnode *, varnode_cmp_def_loc> varnode_def_set;

struct pcodeop_cmp_def {
    bool operator() ( const pcodeop *a, const pcodeop *b ) const;
};

struct pcodeop_domdepth_cmp {
    bool operator() ( const pcodeop *a, const pcodeop *b ) const;
};

typedef set<pcodeop *, pcodeop_cmp_def> pcodeop_def_set;

struct pcodeop_cmp {
    bool operator() ( const pcodeop *a, const pcodeop *b ) const;
};

typedef set<pcodeop *, pcodeop_cmp> pcodeop_set;

struct varnode_cmp_gvn {
    bool operator()(const varnode *a, const varnode *b) const;
};

typedef map<varnode *, vector<pcodeop *>, varnode_cmp_gvn> varnode_gvn_map;

struct coverblock {
	short	version;
	short	blk_index;
	/* 这个结构主要参考自Ghidra的CoverBlock，之所以start和end，没有采用pcodeop结构是因为
	我们在优化时，会删除大量的pcodeop，这个pcode很容易失效 */
	int		start = -1;
	int		end = -1;

	coverblock() {}
	~coverblock() {}

	void set_begin(pcodeop *op);
	void set_end(pcodeop *op);
	void set_end(int);
	bool empty() {
		return (start == -1) && (end == -1);
	}

	bool contain(pcodeop *op);
	void set_all() {
		start = 0;
		end = INT_MAX;
	}
	int dump(char *buf);
};

struct cover {
	map<int, coverblock> c;

	void clear() { c.clear(); }
	void add_def_point(varnode *vn);
	void add_ref_point(pcodeop *op, varnode *vn, int exclude);
	void add_ref_recurse(flowblock *bl);
	int dump(char *buf);
};

struct varnode {
    /* varnode的值类型和值，在编译分析过后就不会被改*/
    valuetype   type;

    struct {
        unsigned    mark : 1;
        unsigned    annotation : 1;
        unsigned    input : 1;          // 没有祖先
        unsigned    written : 1;       // 是def
        unsigned    insert : 1;         // 这个
        unsigned    implied : 1;        // 是一个临时变量
        unsigned    exlicit : 1;        // 不是临时变量

        unsigned    readonly : 1;

        unsigned    covertdirty : 1;    // cover没跟新
        unsigned    virtualnode : 1;     // 虚拟节点，用来做load store分析用
    } flags = { 0 };

    int size = 0;
    int create_index = 0;
    Address loc;

    pcodeop     *def = NULL;
    uintb       nzm;
    /* ssa的版本号，方便定位 */
    int         version = -1;

    varnode_loc_set::iterator lociter;  // sort by location
    varnode_def_set::iterator defiter;  // sort by definition

	cover				cover;
	coverblock			simple_cover;
    list<pcodeop *>     uses;    // descend, Ghidra把这个取名为descend，搞的我头晕，改成use

    varnode(int s, const Address &m);
    ~varnode();

    const Address &get_addr(void) const { return (const Address &)loc; }
    bool            is_heritage_known(void) const { return (flags.insert | flags.annotation) || is_constant(); }
    bool            has_no_use(void) { return uses.empty(); }

    void            set_def(pcodeop *op);
    pcodeop*        get_def() { return def; }
    bool            is_constant(void) const { return type.height == a_constant; }
    bool            in_constant_space() { return get_addr().isConstant(); }
    void            set_val(intb v) { type.height = a_constant;  type.v = v; }
    bool            is_rel_constant(void) { return type.height == a_rel_constant; }
    bool            is_input(void) { return flags.input; }
    void            set_rel_constant(Address &r, int v) { type.height = a_rel_constant; type.v = v;  type.rel = r; }
    intb            get_val(void);
    Address         &get_rel(void) { return type.rel; }

    void            add_use(pcodeop *op);
    void            del_use(pcodeop *op);
    bool            is_free() { return !flags.written && !flags.input; }
    /* 实现的简易版本的，判断某条指令是否在某个varnode的活跃范围内 */
    bool            in_liverange(pcodeop *p);
	bool			in_liverange_simple(pcodeop *p);
    /* 判断在某个start-end之间，这个varnode是否live, start, end必须得在同一个block内

    这2个判断liverange的代码都要重新写
    */
    bool            in_liverange(pcodeop *start, pcodeop *end);
    bool            is_reg() { return get_addr().getSpace()->getType() == IPTR_PROCESSOR; }
	void			add_def_point() { cover.add_def_point(this);  }
	void			add_ref_point(pcodeop *p) { cover.add_ref_point(p, this, 0); }
	void			add_def_point_simple();
	void			add_ref_point_simple(pcodeop *p);
	void			clear_cover_simple();
	void			clear_cover() { cover.clear();  }
	int				dump_cover(char *buf) { 
		int n = simple_cover.dump(buf);
		return n + cover.dump(buf + n);
	}
};

#define PCODE_DUMP_VAL              0x01
#define PCODE_DUMP_UD               0x02
#define PCODE_DUMP_DEAD             0x04
/* 有些def的值被use的太多了，可能有几百个，导致整个cfg图非常的不美观，可以开启这个标记，打印cfg时，只打印部分use，
具体多少，可以参考print_udchain的值
*/

#define PCODE_OMIT_MORE_USE         0x08            
#define PCODE_OMIT_MORE_DEF         0x10            
#define PCODE_OMIT_MORE_BUILD       0x20            
#define PCODE_OMIT_MORE_IN          0x40
#define PCODE_HTML_COLOR            0x80

#define PCODE_DUMP_ALL              ~(PCODE_OMIT_MORE_USE | PCODE_OMIT_MORE_DEF | PCODE_OMIT_MORE_BUILD | PCODE_OMIT_MORE_IN)
#define PCODE_DUMP_SIMPLE           0xffffffff

struct pcodeop {
    struct {
        unsigned startblock : 1;
        unsigned branch : 1;
        unsigned call : 1;
        unsigned returns: 1;
        unsigned nocollapse : 1;
        unsigned dead : 1;
        unsigned marker : 1;        // 特殊的站位符， (phi 符号 或者 间接引用 或 CPUI_COPY 对同一个变量的操作)，
        unsigned boolouput : 1;     // 布尔操作

        unsigned coderef : 1;
        unsigned startinst : 1;     // instruction的第一个pcode
        /* 临时算法有用:
        1. compute_sp
        */
        unsigned mark : 1;          // 临时性标记，被某些算法拿过来做临时性处理，处理完都要重新清空

        unsigned branch_call : 1;   // 一般的跳转都是在函数内进行的，但是有些壳的函数，会直接branch到另外一个函数里面去
        unsigned exit : 1;          // 这个指令起结束作用
        unsigned inlined : 1;       // 这个opcode已经被inline过了
        unsigned changed : 1;       // 这个opcode曾经被修改过
        unsigned input : 1;         // input有2种，一种是varnode的input，代表这个寄存器来自于
        unsigned copy_from_phi : 1;           // 给opcode为cpy的节点使用，在删除只有2个入边的join node时，会导致这个节点的phi节点修改成
                                    // copy，这里要标识一下
        unsigned vm_vis : 1;        // 给vm做标记用的
        unsigned vm_eip : 1;
        unsigned zero_load : 1;         // 0地址访问
        unsigned force_constant : 1;    // 强制常量，用来在某些地方硬编码时，不方便计算，人工计算后，强行填入
        unsigned trace : 1;
            /* 
            FIXME:
            会影响load行为的有2种opcode
            1. store
            2. sp = sp - xxx
            后面一种opcode，我们假设alloc出的内存空间值都是0，这个是有问题的?
            */
        unsigned val_from_sp_alloc : 1;     // 这个load的值并非来自于store，而是来自于sp的内存分配行为
		unsigned uncalculated_store : 1;	// 这个store节点是不可计算的
    } flags = { 0 };

    OpCode opcode;
    /* 一个指令对应于多个pcode，这个是用来表示inst的 */
    SeqNum start;
    flowblock *parent;
    /* 我们认为程序在分析的时候，sp的值是可以静态分析的，他表示的并不是sp寄存器，而是系统当前堆栈的深度 */
    int     sp = 0;
    Address *disaddr = NULL;

    varnode *output = NULL;
    vector<varnode *> inrefs;

    funcdata *callfd = NULL;   // 当opcode为call指令时，调用的

    list<pcodeop *>::iterator basiciter;
    list<pcodeop *>::iterator insertiter;
    list<pcodeop *>::iterator codeiter;
    list<pcodeop *> mayuses;

    pcodeop(int s, const SeqNum &sq);
    ~pcodeop();

    void            set_opcode(OpCode op);
    varnode*        get_in(int slot) { return inrefs[slot];  }
    varnode*        get_in(const Address &addr) {
        for (int i = 0; i < inrefs.size(); i++) {
            if (inrefs[i]->get_addr() == addr) return inrefs[i];
        }

        return NULL;
    }
    varnode*        get_out() { return output;  }
    const Address&  get_addr() { return start.getAddr();  }
    /* dissasembly 时用到的地址 */
    const Address&  get_dis_addr(void) { return disaddr ? disaddr[0] : get_addr(); }

    int             num_input() { return inrefs.size();  }
    void            clear_input(int slot);
    void            remove_input(int slot);
    void            insert_input(int slot);

    void            set_input(varnode *vn, int slot) { inrefs[slot] = vn; }
    int             get_slot(const varnode *vn) { 
        int i, n; n = inrefs.size(); 
        for (i = 0; i < n; i++)
            if (inrefs[i] == vn) return i;
        return -1;
    }
    int             dump(char *buf, uint32_t flags);
    /* trace compute 
    这个compute_t 和传统的compute不一样，普通的comupte只能用来做常量传播，这个compute_t
    是在循环展开时，沿某条路径开始计算

    compute_t 和 模拟执行是完全不一样的， 模拟执行会给所有系统寄存器赋值，然后走入函数，
    compute_t 不会做这样，compute_t是在某条路径上执行，假如可以计算，就计算，假如不能计算
    就跳过

@inslot         常量传播时，填-1，假如在做trace分析时，填从哪个快进入
@return     
            1       unknown bcond
    */

    /* 碰见了可以计算出的跳转地址 */
#define         ERR_MEET_CALC_BRANCH            1
#define         ERR_UNPROCESSED_ADDR            2
#define         ERR_CONST_CBRANCH               4
#define         ERR_FREE_SELF                   8
    /* 

    branch:         计算的时候发现可以跳转的地址
    wlist:          工作表，当我们跟新某些节点的时候，发现另外一些节点也需要跟新，就把它加入到这个链表内
    */
    int             compute(int inslot, flowblock **branch);
	int				compute_add_sub();
    void            set_output(varnode *vn) { output = vn;  }

    bool            is_dead(void) { return flags.dead;  }
    bool            have_virtualnode(void) { return inrefs.size() == 3;  }
    varnode*        get_virtualnode(void) { return inrefs.size() == 3 ? inrefs[2]:NULL;  }
    bool            is_call(void) { return (opcode == CPUI_CALL) || (opcode == CPUI_CALLIND) || callfd; }
    void            set_input() { flags.input = 1;  }
    intb            get_call_offset() { return get_in(0)->get_addr().getOffset(); }
    /* 当自己的结果值为output时，把自己整个转换成copy形式的constant */
    void            to_constant(void);
    void            to_rel_constant(void);
    void            to_copy(varnode *in);
    /* 转换成nop指令 */
    void            to_nop(void);
    void            add_mayuse(pcodeop *p) { mayuses.push_back(p);  }
    bool            is_trace() { return flags.trace;  }
    void            set_trace() { flags.trace = 1; }
    void            clear_trace() { flags.trace = 0;  }
    /* in的地址是否在sp alloc内存的位置 */
    bool            in_sp_alloc_range(varnode *in);
    void            peephole(void);
};

typedef struct blockedge            blockedge;

#define a_tree_edge             0x1
#define a_forward_edge          0x2
#define a_cross_edge            0x4
#define a_back_edge             0x8
#define a_loop_edge             0x10
#define a_true_edge             0x20
#define a_mark                  0x40

struct blockedge {
    int label;
    flowblock *point;
    int reverse_index;

    blockedge(flowblock *pt, int lab, int rev) { point = pt, label = lab; reverse_index = rev; }
    blockedge() {};
    bool is_true() { return label & a_true_edge;  }
    void set_true(void) { label |= a_true_edge; }
    void set_false(void) { label &= a_true_edge;  }
};


enum block_type{
    a_condition,
    a_if,
    a_whiledo,
    a_dowhile,
    a_switch,
};

/* 模拟stack行为，*/
struct mem_stack {

    int size;
    char *data;
    
    mem_stack();
    ~mem_stack();

    void    push(char *byte, int size);
    int     top(int size);
    int     pop(int size);
};

struct flowblock {
    enum block_type     type;

    struct {
        unsigned f_goto_goto : 1;
        unsigned f_break_goto : 1;
        unsigned f_continue_goto : 1;
        unsigned f_switch_out : 1;
        unsigned f_entry_point : 1;
        /* 
        1. 在cbranch中被分析为不可达，确认为死 */
        unsigned f_dead : 1; 

        unsigned f_switch_case : 1;
        unsigned f_switch_default : 1;

        /* 在某些算法中，做临时性标记用 

        1. reachable测试
        2. reducible测试
        3. augment dominator tree收集df
        4. djgraph 收集df
        */
        unsigned f_mark : 1;

        unsigned f_return : 1;

        /* 这个block快内有call函数 */
        unsigned f_call : 1;
        /* 不允许合并 */
        unsigned f_unsplice : 1;
        /* 内联的时候碰到的cbranch指令 */
        unsigned f_cond_cbranch : 1;
        /* 我们假设节点 e 为结束节点，节点 a -> e，当且仅当e为a的唯一出节点，那么a在e的结束路径上，
        e也在自己的路径上
        */
        unsigned f_exitpath : 1;
        /* 这个循环*/
        unsigned f_irreducible : 1;
        unsigned f_loopheader : 1;
    } flags = { 0 };

    RangeList cover;

    list<pcodeop*>      ops;

    flowblock *parent = NULL;
    flowblock *immed_dom = NULL;
    /* 
    1. 标明自己属于哪个loop
    2. 假如自己哪个loop都不属于，就标空
    3. 假如一个循环内有多个节点，找dfnum最小的节点
    4. 对于一个循环内的除了loopheader的节点，这个指针指向loopheader，loopheader自己本身的
       loopheader指向的起始是外层的loop的loopheader，这个一定要记住
    */
    flowblock *loopheader = NULL;
    /* 标明这个loop有哪些节点*/
    vector<flowblock *> irreducibles;
    vector<flowblock *> loopnodes;
    /* 识别所有的循环头 */
    vector<flowblock *> loopheaders;
    /* 
    1. 测试可规约性
    2. clone web时有用
    */
    flowblock *copymap = NULL;

    /* 这个index是 反后序遍历的索引，用来计算支配节点数的时候需要用到 */
    int index = 0;
    int dfnum = 0;
    int numdesc = 1;        // 在 spaning tree中的后代数量，自己也是自己的后代，所以假如正式计算后代数量，自己起始为1

    int vm_byteindex = -1;      
    int vm_caseindex = -1;

    vector<blockedge>   in;
    vector<blockedge>   out;
    vector<flowblock *> blist;
    /* 一个函数的所有结束节点 */
    vector<flowblock *> exitlist;
    /* 有些block是不可到达的，都放到这个列表内 */
    vector<flowblock *> deadlist;

    jmptable *jmptable = NULL;

    funcdata *fd;

    flowblock(funcdata *fd);
    ~flowblock();

    void        add_block(flowblock *b);
    blockbasic* new_block_basic(funcdata *f);
    flowblock*  get_out(int i) { return out[i].point;  }
    flowblock*  get_in(int i) { return in[i].point;  }
    flowblock*  get_block(int i) { return blist[i]; }
    flowblock*  get_block_by_index(int index) {
        for (int i = 0; i < blist.size(); i++)
            if (blist[i]->index == index) return blist[i];

        return NULL;
    }
    pcodeop*    first_op(void) { return *ops.begin();  }
    pcodeop*    last_op(void) { 
		return ops.size() ? (*--ops.end()):NULL;  
	}
    int         get_out_rev_index(int i) { return out[i].reverse_index;  }

    void        set_start_block(flowblock *bl);
    void        set_initial_range(const Address &begin, const Address &end);
    void        add_op(pcodeop *);
    void        insert(list<pcodeop *>::iterator iter, pcodeop *inst);

    int         sub_id();
    void        structure_loops(vector<flowblock *> &rootlist);
    void        find_spanning_tree(vector<flowblock *> &preorder, vector<flowblock *> &rootlist);
    void        dump_spanning_tree(const char *filename, vector<flowblock *> &rootlist);
    void        calc_forward_dominator(const vector<flowblock *> &rootlist);
    void        build_dom_tree(vector<vector<flowblock *> > &child);
    int         build_dom_depth(vector<int> &depth);
    /*
    寻找一种trace流的反向支配节点，

    一般的反向支配节点算法，就是普通支配节点算法的逆

    而这个算法是去掉，部分节点的回边而生成反向支配节点，用来在trace流中使用
    */
    flowblock*  find_post_tdom(flowblock *h);
    bool        find_irreducible(const vector<flowblock *> &preorder, int &irreduciblecount);
    void        calc_loop();

    int         get_size(void) { return blist.size();  }
    Address     get_start(void);

    bool        is_back_edge_in(int i) { return in[i].label & a_back_edge; }
    void        set_mark() { flags.f_mark = 1;  }
    void        clear_mark() { flags.f_mark = 0;  }
    void        clear_marks(void);
    bool        is_mark() { return flags.f_mark;  }
    bool        is_entry_point() { return flags.f_entry_point;  }
    bool        is_switch_out(void) { return flags.f_switch_out;  }
    flowblock*  get_entry_point(void);
    int         get_in_index(const flowblock *bl);
    int         get_out_index(const flowblock *bl);
    void        calc_exitpath();

    void        clear(void);
    int         remove_edge(flowblock *begin, flowblock *end);
    void        add_edge(flowblock *begin, flowblock *end);
    void        add_edge(flowblock *b, flowblock *e, int label);
    void        add_in_edge(flowblock *b, int lab);
    int         remove_in_edge(int slot);
    void        remove_out_edge(int slot);
    void        half_delete_out_edge(int slot);
    void        half_delete_in_edge(int slot);
    int         get_back_edge_count(void);
    flowblock*  get_back_edge_node(void);
    /* 当这个block的末尾节点为cbranch节点时，返回条件为真或假的跳转地址 */
    blockedge*  get_true_edge(void);
    blockedge*  get_false_edge(void);

    void        set_out_edge_flag(int i, uint4 lab);
    void        clear_out_edge_flag(int i, uint4 lab);

    int         get_inslot(flowblock *inblock) {
        for (int i = 0; i < in.size(); i++) {
            if (in[i].point == inblock)
                return i;
        }

        return -1;
    }

    void        set_dead(void) { flags.f_dead = 1;  }
    int         is_dead(void) { return flags.f_dead;  }
    bool        is_irreducible() { return flags.f_irreducible;  }
    void        remove_from_flow(flowblock *bl);
    void        remove_op(pcodeop *inst);
    void        remove_block(flowblock *bl);
    void        collect_reachable(vector<flowblock *> &res, flowblock *bl, bool un) const;
    void        splice_block(flowblock *bl);
    void        move_out_edge(flowblock *blold, int slot, flowblock *blnew);
    void        replace_in_edge(int num, flowblock *b);
    list<pcodeop *>::reverse_iterator get_rev_iterator(pcodeop *op);
    flowblock*  add_block_if(flowblock *b, flowblock *cond, flowblock *tc);
    bool        is_dowhile(flowblock *b);
    pcodeop*    first_callop();
    /* 搜索到哪个节点为止 */
    pcodeop*    first_callop_vmp(flowblock *end);
    /* 这个函数有点问题 */
    flowblock*  find_loop_exit(flowblock *start, flowblock *end);

    /*
    1. 检测header是否为 while...do 形式的循环的头节点
    2. 假如不是，返回NULL
    3. 假如是，计算whiledo的结束节点是哪个
    */
    flowblock*  detect_whiledo_exit(flowblock *header);
    void        mark_unsplice() { flags.f_unsplice = 1;  }
    bool        is_unsplice() { return flags.f_unsplice; }
    bool        is_end() { return out.size() == 0;  }
    Address     get_return_addr();
    void        clear_all_unsplice();
    void        clear_all_vminfo();
    void        add_loopheader(flowblock *b) { 
        b->flags.f_loopheader = 1;
        loopheaders.push_back(b);  
        b->loopnodes.push_back(b);
        b->loopheader;
    }
    bool        in_loop(flowblock *lheader, flowblock *node);
    void        clear_loopinfo() {
        loopheader = NULL;
        loopheaders.clear();
        loopnodes.clear();
        irreducibles.clear();
        flags.f_irreducible = 0;
        flags.f_loopheader = 0;
    }
    pcodeop*    get_pcode(int pid) {
        list<pcodeop *>::iterator it;
        for (it = ops.begin(); it != ops.end(); it++) {
            if ((*it)->start.getTime() == pid)
                return *it;
        }

        return NULL;
    }
    /* 查找以这个变量为out的第一个pcode */
    pcodeop*    find_pcode_def(const Address &out);
};

typedef struct priority_queue   priority_queue;

struct priority_queue {
    vector<vector<flowblock *> > queue;
    int curdepth;

    priority_queue(void) { curdepth = -2;  }
    void reset(int maxdepth);
    void insert(flowblock *b, int depth);
    flowblock *extract();
    bool empty(void) const { return (curdepth == -1);  }
};

typedef map<SeqNum, pcodeop *>  pcodeop_tree;
typedef struct op_edge      op_edge;
typedef struct jmptable     jmptable;

struct op_edge {
    pcodeop *from;
    pcodeop *to;
    int t = 0;

    op_edge(pcodeop *from, pcodeop *to);
    ~op_edge();
} ;

struct funcproto {
    struct {
        unsigned vararg : 1;        // variable argument
        unsigned exit : 1;          // 调用了这个函数会导致整个流程直接结束，比如 exit, stack_check_fail
        unsigned side_effect : 1;
    } flags = { 0 };
    /* -1 代表不知道 
    */
    int     inputs = -1;
    int     output = -1;
    string  name;
    Address addr;

    funcproto() { flags.side_effect = 1;  }
    ~funcproto() {}

    void set_side_effect(int v) { flags.side_effect = v;  }
};

struct jmptable {
    pcodeop *op;
    Address opaddr;
    int defaultblock;
    int lastblock;
    int size;

    vector<Address>     addresstable;

    jmptable(pcodeop *op);
    jmptable(const jmptable *op2);
    ~jmptable();

    void    update(funcdata *fd);
};

typedef funcdata* (*test_cond_inline_fn)(dobc *d, intb addr);

struct rangenode {
    intb    start = 0;
    int     size = 0;

    rangenode();
    ~rangenode();

    intb    end() { return start + size;  }
};

struct funcdata {
    struct {
        unsigned blocks_generated : 1;
        unsigned blocks_unreachable : 1;    // 有block无法到达
        unsigned processing_started : 1;
        unsigned processing_complete : 1;
        unsigned no_code : 1;
        unsigned unimplemented_present : 1;
        unsigned baddata_present : 1;

        unsigned safezone : 1;
        unsigned plt : 1;               // 是否是外部导入符号
        unsigned exit : 1;              // 有些函数有直接结束整个程序的作用，比如stack_check_fail, exit, abort
		/* 是否允许标记未识别store，让安全store可以跨过去这个pcode*/
		unsigned enable_topstore_mark : 1;
		/* liverange有2种计算类型
		
		1. 一种是快速但不完全，可以做peephole，不能做register allocation
		2. 一种是慢速但完全，可以参与所有优化 */
		unsigned enable_complete_liverange : 1;
    } flags = { 0 };

    enum {
        a_local,
        a_global,
        a_plt,
    } symtype;

    int op_generated = 0;
	int reset_version = 0;

    pcodeop_tree     optree;
    AddrSpace   *uniq_space = NULL;
    funcproto       funcp;

    struct {
        funcdata *next = NULL;
        funcdata *prev = NULL;
    } node;

    list<op_edge *>    edgelist;

    /* jmp table */
    vector<pcodeop *>   tablelist;
    vector<jmptable *>  jmpvec;

    /* op_gen_iter 用来分析ops时用到的，它指向上一次分析到的pcode终点 */
    list<pcodeop *>::iterator op_gen_iter;
    /* deadlist用来存放所有pcode */
    list<pcodeop *>     deadlist;
    list<pcodeop *>     alivelist;
    list<pcodeop *>     storelist;
    list<pcodeop *>     loadlist;
    list<pcodeop *>     useroplist;
    list<pcodeop *>     deadandgone;
    list<pcodeop *>     philist;
    /* 安全的别名信息，可以传播用 */
    list<pcodeop *>     safe_aliaslist;

    /* 我们不能清除顶层名字空间的变量，因为他可能会被外部使用 */
    pcodeop_def_set topname;
    intb user_step = 0x10000;
    intb user_offset = 0x10000;
    int op_uniqid = 0;

    map<Address,VisitStat> visited;
    dobc *d = NULL;
    flowblock * vmhead = NULL;

    /* vbank------------------------- */
    struct {
        long uniqbase = 0;
        int uniqid = 0;
        int create_index = 0;
        struct dynarray all = { 0 };
    } vbank;

    varnode_loc_set     loc_tree;
    varnode_def_set     def_tree;
    varnode             searchvn;
    /* vbank------------------------- */

    /* control-flow graph */
    blockgraph bblocks;

    list<op_edge *>       block_edge;

    int     intput;         // 这个函数有几个输入参数
    int     output;         // 有几个输出参数
    list<func_call_specs *>     qlst;

    /* heritage start ................. */
    vector<vector<flowblock *> > domchild;
    vector<vector<flowblock *> > augment;
#define boundary_node       1
#define mark_node           2
#define merged_node         4
#define visit_node          8
    vector<uint4>   phiflags;   
    vector<int>     domdepth;
    /* dominate frontier */
    vector<flowblock *>     merge;      // 哪些block包含phi节点
    priority_queue pq;

    int maxdepth = -1;

    LocationMap     disjoint;
    LocationMap     globaldisjoint;
    /* FIXME:我不是很理解这个字段的意思，所以我没用他，一直恒为0 */
    int pass = 0;

    /* heritage end  ============================================= */
    vector<pcodeop *>   trace;
    int             virtualbase = 0x10000;
    /*---*/

    Address startaddr;

    Address baddr;
    Address eaddr;
    string fullpath;
    string name;
    string alias;
    int size = 0;

    /* 扫描到的最小和最大指令地址 */
    Address minaddr;
    Address maxaddr;
    int inst_count = 0;
    int inst_max = 1000000;

    /* 这个区域内的所有可以安全做别名分析的点 */

    /* vmp360--------- */
    list<rangenode *> safezone;
    intb     vmeip = 0;
    /* vmp360  end--------- */

    vector<Address>     addrlist;
    /* 常量cbranch列表 */
    vector<pcodeop *>       cbrlist;
    vector<flowblock *>     emptylist;
    pcodeemit2 emitter;

    /* 做条件inline时用到 */
    funcdata *caller = NULL;
    pcodeop *callop = NULL;


    struct {
        int     size;
        u1      *bottom;
        u1      *top;
    } memstack;

    funcdata(const char *name, const Address &a, int size, dobc *d);
    ~funcdata(void);

    const Address&  get_addr(void) { return startaddr;  }
    string&     get_name() { return name;  }
    void        set_alias(string a) { alias = a;  }
    string&     get_alias(void) { return alias;  }
    void        set_range(Address &b, Address &e) { baddr = b; eaddr = e; }
    void        set_op_uniqid(int val) { op_uniqid = val;  }
    int         get_op_uniqid() { return op_uniqid; }
    void        set_user_offset(int v) { user_offset = v;  }
    int         get_user_offset() { return user_offset; }
    void        set_virtualbase(int v) { virtualbase = v;  }
    int         get_virtualbase(void) { return virtualbase; }

    pcodeop*    newop(int inputs, const SeqNum &sq);
    pcodeop*    newop(int inputs, const Address &pc);
    pcodeop*    cloneop(pcodeop *op, const SeqNum &seq);
    void        op_destroy_raw(pcodeop *op);
    void        op_destroy(pcodeop *op);
    void        op_destroy_ssa(pcodeop *op);
	void		op_destroy(pcodeop *op, int remove);
	void		remove_all_dead_op();
    void        reset_out_use(pcodeop *p);

    varnode*    new_varnode_out(int s, const Address &m, pcodeop *op);
    varnode*    new_varnode(int s, AddrSpace *base, uintb off);
    varnode*    new_varnode(int s, const Address &m);
    /* new_coderef是用来创建一些程序位置的引用点的，但是这个函数严格来说是错的····，因为标识函数在pcode
    的体系中，多个位置他们的address可能是一样的
    */
    varnode*    new_coderef(const Address &m);
    varnode*    new_unique(int s);
    varnode*    new_unique_out(int s, pcodeop *op);

    varnode*    clone_varnode(const varnode *vn);
    void        destroy_varnode(varnode *vn);
    void        delete_varnode(varnode *vn);
    /* 设置输入参数 */
    varnode*    set_input_varnode(varnode *vn);

    varnode*    create_vn(int s, const Address &m);
    varnode*    create_def(int s, const Address &m, pcodeop *op);
    varnode*    create_def_unique(int s, pcodeop *op);
    varnode*    create_constant_vn(intb val, int size);
    varnode*    xref(varnode *vn);
    varnode*    set_def(varnode *vn, pcodeop *op);

    void        op_resize(pcodeop *op, int size);
    void        op_set_opcode(pcodeop *op, OpCode opc);
    void        op_set_input(pcodeop *op, varnode *vn, int slot);
    void        op_set_output(pcodeop *op, varnode *vn);
    void        op_unset_input(pcodeop *op, int slot);
    void        op_unset_output(pcodeop *op);
    void        op_remove_input(pcodeop *op, int slot);
    void        op_insert_input(pcodeop *op, varnode *vn, int slot);
    void        op_zero_multi(pcodeop *op);
    void        op_unlink(pcodeop *op);
    void        op_uninsert(pcodeop *op);
    void        clear_block_phi(flowblock *b);
    void        clear_block_df_phi(flowblock *b);

    pcodeop*    find_op(const Address &addr);
    pcodeop*    find_op(const SeqNum &num) const;
    void        del_op(pcodeop *op);
    void        del_varnode(varnode *vn);

    varnode_loc_set::const_iterator     begin_loc(const Address &addr);
    varnode_loc_set::const_iterator     end_loc(const Address &addr);
    varnode_loc_set::const_iterator     begin_loc(AddrSpace *spaceid);
    varnode_loc_set::const_iterator     end_loc(AddrSpace *spaceid);

    void        del_remaining_ops(list<pcodeop *>::const_iterator oiter);
    void        new_address(pcodeop *from, const Address &to);
    pcodeop*    find_rel_target(pcodeop *op, Address &res) const;
    pcodeop*    target(const Address &addr) const;
    pcodeop*    branch_target(pcodeop *op);
    pcodeop*    fallthru_op(pcodeop *op);

    bool        set_fallthru_bound(Address &bound);
    void        fallthru();
    pcodeop*    xref_control_flow(list<pcodeop *>::const_iterator oiter, bool &startbasic, bool &isfallthru);
    void        generate_ops_start(void);
    void        generate_ops(void);
    bool        process_instruction(const Address &curaddr, bool &startbasic);
    void        recover_jmptable(pcodeop *op, int indexsize);
    void        analysis_jmptable(pcodeop *op);
    jmptable*   find_jmptable(pcodeop *op);

    void        collect_edges();
    void        generate_blocks();
    void        split_basic();
    void        connect_basic();

    void        dump_inst();
    void        dump_block(FILE *fp, blockbasic *b, int pcode);
    /* flag: 1: enable pcode */
    void        dump_cfg(const string &name, const char *postfix, int flag);
    void        dump_pcode(const char *postfix);
    /* 打印loop的包含关系 */
    void        dump_loop(const char *postfix);
    /* dump dom-joint graph */
    void        dump_djgraph(const char *postfix, int flag);
	void        dump_liverange(const char *postfix);

    void        op_insert_before(pcodeop *op, pcodeop *follow);
    void        op_insert_after(pcodeop *op, pcodeop *prev);
    void        op_insert(pcodeop *op, blockbasic *bl, list<pcodeop *>::iterator iter);
    void        op_insert_begin(pcodeop *op, blockbasic *bl);
    void        op_insert_end(pcodeop *op, blockbasic *bl);
    void        inline_flow(funcdata *inlinefd, pcodeop *fd);
    void        inline_clone(funcdata *inelinefd, const Address &retaddr);
    void        inline_call(string name, int num);
    void        inline_call(const Address &addr, int num);
    /* 条件inline
    
    当我们需要inline一个函数的时候，某些时候可能不需要inline他的全部代码，只inline部分

    比如 一个vmp_ops函数

    function vmp_ops(VMState *s, int val1, int val2) {
        optype = stack_top(s);
        if (optype == 17) {
            vmp_ops2(s, val1, val2);
        }
        else if (optype == 16) {
        }
        else {
        }
    }

    假如我们外层代码在调用vmp_ops的时候，代码如下

    stack_push(s, 17);
    vmp_ops(s, 1, 2);

    那么实际上vmp_ops进入以后，只会进入17的那个分支，我们在inline一个函数时，
    把当前的上下文环境传入进去(记住这里不是模拟执行)，然后在vmp_ops内做编译优化，
    并对它调用的函数，也做同样的cond_inline。

    这种奇怪的优化是用来对抗vmp保护的，一般的vmp他们在解构函数时，会形成大量的opcode
    然后这些opcode，理论上是可以放到一个大的switch里面处理掉的，有些写壳的作者会硬是把
    这个大的switch表拆成多个函数
    */
    flowblock*  argument_inline(funcdata *inlinefd, pcodeop *fd);
    void        argument_pass(void);
    void        set_caller(funcdata *caller, pcodeop *callop);

    void        inline_ezclone(funcdata *fd, const Address &calladdr);
    bool        check_ezmodel(void);
    void        structure_reset();

    void        mark_dead(pcodeop *op);
    void        mark_alive(pcodeop *op);
    void        mark_free(varnode *vn);
    void        fix_jmptable();
    char*       block_color(flowblock *b);
    char*       edge_color(blockedge *e);
    int         edge_width(blockedge *e);
    void        start_processing(void);
    void        follow_flow(void);
    void        add_callspec(pcodeop *p, funcdata *fd);
    void        clear_blocks();
    void        clear_blocks_mark();
    int         inst_size(const Address &addr);
    void        build_adt(void);
    void        calc_phi_placement(const vector<varnode *> &write);
    void        calc_phi_placement2(const vector<varnode *> &write);
    void        calc_phi_placement3(const vector<flowblock *> &write);
    void        visit_dj(flowblock *cur,  flowblock *v);
    void        visit_incr(flowblock *qnode, flowblock *vnode);
    void        place_multiequal(void);
    void        rename();
    void        rename_recurse(blockbasic *bl, variable_stack &varstack, version_map &vermap);
	void		build_liverange();
    void        build_liverange_recurse(blockbasic *bl, variable_stack &varstack);

    int         collect(Address addr, int size, vector<varnode *> &read,
        vector<varnode *> &write, vector<varnode *> &input);
    void        heritage(void);
    void        heritage_clear(void);
    int         constant_propagation3();
    int         cond_constant_propagation();
    int         in_cbrlist(pcodeop *op) {
        for (int i = 0; i < cbrlist.size(); i++) {
            if (cbrlist[i] == op)
                return 1;
        }

        return 0;
    }
    /* compute sp要计算必须得满足2个要求
    
    1. block 已经被generated
    2. constant_propagation 被执行过
    */
    void        compute_sp(void);
    bool        is_code(varnode *v, varnode *v1);
    bool        is_sp_rel_constant(varnode *v);

    void        set_safezone(intb addr, int size);
    bool        in_safezone(intb addr, int size);
    void        enable_safezone(void);
    void        disable_safezone(void);

    intb        get_stack_value(intb offset, int size);
    void        set_stack_value(intb offset, int size, intb val);
    void        add_to_codelist(pcodeop *op);
    void        remove_from_codelist(pcodeop *op);

    void        set_plt(int v) { flags.plt = v; };
    void        set_exit(int v) { flags.exit = v; }
    bool        test_hard_inline_restrictions(funcdata *inlinefd, pcodeop *op, Address &retaddr);
    bool        is_first_op(pcodeop *op);

    /* 获取loop 的头节点的in 节点，假如有多个，按index顺序取一个 */
    flowblock*  loop_pre_get(flowblock *h, int index);
    bool        trace_push(pcodeop *op);
    void        trace_push_op(pcodeop *op);
    void        trace_clear();
    pcodeop*    trace_store_query(pcodeop *load);
    /* 查询某个load是来自于哪个store，有2种查询方式，
    
    一种是直接指明load，后面的b可以填空，从这个load开始往上搜索
    一种是不指明load，但是指明b，从这个block开始搜索

    @load       要从这条load开始搜索，pos也来自于这个load
    @b          要从这个block开始搜搜，pos来自于外部提供，b的优先级低于load
    @pos        要搜索位置
    @maystore   当发现无法判断的store，返回这个store
    */
    pcodeop*    store_query(pcodeop *load, flowblock *b, varnode *pos, pcodeop **maystore);

    /* 基于安全区域的store搜索 */
    pcodeop*    store_query2(pcodeop *load, flowblock *b, varnode *pos, pcodeop **maystore);
#define _DUMP_PCODE             0x01
#define _DUMP_ORIG_CASE         0x02
    /* 循环展开的假如是 while switch case 里的分支则需要clone，假如不是的话则不需要复制后面的流 */
#define _DONT_CLONE             0x08
#define _NOTE_VMBYTEINDEX       0x10 
    bool        loop_unrolling4(flowblock *h, int times, uint32_t flags);

    /* 搜索从某个节点开始到某个节点的，所有in节点的集合 */
    int         collect_blocks_to_node(vector<flowblock *> &blks, flowblock *start, flowblock *end);
    /*

    @h          起始节点
    @enter      循环展开的头位置
    @end        循环展开的结束位置，不包含end，
                当循环粘展开到最后一个节点，跳出循环时，终止节点就变成了exit节点
    */
    flowblock*  loop_unrolling(flowblock *h, flowblock *end, uint32_t flags, int &meet_exit);
    /* 这里的dce加了一个数组参数，用来表示只有当删除的pcode在这个数组里才允许删除 这个是为了方便调试以及还原 */
#define RDS_0           1
#define RDS_UNROLL0     2

    void        dead_code_elimination(vector<flowblock *> blks, uint32_t flags);
    flowblock*  get_vmhead(void);
    flowblock*  get_vmhead_unroll(void);
    pcodeop*    get_vmcall(flowblock *b);

    bool        use_outside(varnode *vn);
    void        use2undef(varnode *vn);
    void        branch_remove(blockbasic *bb, int num);
	/* 把bb和第num个节点的关系去除 */
    void        branch_remove_internal(blockbasic *bb, int num);
    void        block_remove_internal(blockbasic *bb, bool unreachable);
    bool        remove_unreachable_blocks(bool issuewarnning, bool checkexistence);
    void        splice_block_basic(blockbasic *bl);
    void        remove_empty_block(blockbasic *bl);

    void        redundbranch_apply();
    void        dump_store_info(const char *postfix);
    void        dump_load_info(const char *postfix);

    /* 循环展开时用，从start节点开始，搜索start可以到的所有节点到 end为止，全部复制出来
    最后的web包含start，不包含end */
    flowblock*  clone_web(flowblock *start, flowblock *end, vector<flowblock *> &cloneblks);
    flowblock*  clone_ifweb(flowblock *newstart, flowblock *start, flowblock *end, vector<flowblock *> &cloneblks);
	flowblock*	inline_call(pcodeop *callop, funcdata *fd);
#define F_OMIT_RETURN       1
    flowblock*  clone_block(flowblock *f, u4 flags);
    /* 把某个block从某个位置开始切割成2块，

    比如

    1. mov r0, 1
    2. call xxx
    3. mov r1, r0
    我们内联inline了xxx以后，xxx可能是 if .. else 的结构
    那么我们需要把整个快拆开
    */
    flowblock*  split_block(flowblock *f, list<pcodeop *>::iterator it);

    char*       get_dir(char *buf);

    bool        have_side_effect(void) { return funcp.flags.side_effect;  }
    bool        have_side_effect(pcodeop *op, varnode *pos);
    void        alias_clear(void);
    /* 循环展开时用
    
    do {
        inst1
    } while (cond)

    转成

    inst1
    if (cond) {
        do {
            inst1
        } while (cond)
    }

    然后inst1在解码完以后，会得出cond的条件，假如为真，则继续展开

    inst1
    inst1
    if (cond) {
        do {
            inst1
        } while (cond)
    }
    一直到cond条件为假，删除整个if块即可
    */
    flowblock*  dowhile2ifwhile(vector<flowblock *> &dowhile);
    char*       print_indent();
    /* 跟严格的别名测试 */
    bool        test_strict_alias(pcodeop *load, pcodeop *store);
    /* 删除死去的store

    以前的代码，一次只能删除一个死去的store

    新的版本，会递归删除
    */
    void        remove_dead_store2(flowblock *b, map<valuetype, vector<pcodeop *> > &m);
    void        remove_dead_store(flowblock *b);
    void        remove_dead_stores();
    /* 打印某个节点的插入为止*/
    void        dump_phi_placement(int bid, int pid);
    /* 搜索归纳变量 */
    varnode*    detect_induct_variable(flowblock *h, flowblock *&exit);
    bool        can_analysis(flowblock *b);

    /* 
    
    把一个 
    a->h
    b->h的结构转换成
    a->c->h
    b->c->h
    @return     c
    */
    flowblock*  combine_multi_in_before_loop(vector<flowblock *> ins, flowblock *header);
    void        dump_exe();
    /* 检测可计算循环 */
    void        detect_calced_loops(vector<flowblock *> &loops);
/* 找到某个循环出口活跃的变量集合 */
    void        remove_loop_livein_varnode(flowblock *lheader);
    void        remove_calculated_loop(flowblock *lheader);
    void        remove_calculated_loops();

    /* 针对不同的加壳程序生成不同的vmeip检测代码 */
    bool        vmp360_detect_vmeip();
    /* FIXME:应该算是代码中最重的硬编码，有在尝试去理解整个VMP框架的堆栈部分，

    360的vmp堆栈入口部分分为以下几部分:

    1. call convection prilogue save
       stmdb sp!,{r4 r5 r6 lr}
       入口部分，保护寄存器
    2. local variable 
       sub sp,sp,#0x30
       上面的0x30可能是任意值，应该是原始函数分配的堆栈大小，后面的0x100，应该是框架额外的扩展
    3. vmp framework extend
       sub sp,sp,#0x100
       vmp框架扩展的
    4. save sp 
    5. save all regs except sp
    6. save cpsr
    7. save cpsr
    8. call convections prilogue save
       7, 8都处理了2次，不是很懂360想做什么
    9. 开辟了vmp的 stack
       sub sp,sp,#0x34

    结束，大概vmp进入函数的堆栈基本就是这样
    */
    int         vmp360_detect_safezone();
    int         vmp360_detect_framework_info();
    int         vmp360_deshell();
    /* 标注程序中的堆栈中，某些360的重要字段，方便分析，这个只是在前期的debug有用，
    实际优化中是用不到的，所以它不属于硬编码 */
    void        vmp360_marker(pcodeop *op);
};

struct func_call_specs {
    pcodeop *op;
    funcdata *fd;

    func_call_specs(pcodeop *o, funcdata *f);
    ~func_call_specs();

    const string &get_name(void) { return fd->name;  }
    const Address &get_addr() { return fd->get_addr(); }
};

struct dobc {
    ElfLoadImage *loader;
    string slafilename;

    string fullpath;
    string filename;

    ContextDatabase *context = NULL;
    Translate *trans = NULL;
    TypeFactory *types;

    struct {
        int counts = 0;
        funcdata *list = NULL;
    } funcs;

    int max_basetype_size;
    int min_funcsymbol_size;
    int max_instructions;
    map<string, string>     abbrev;
    test_cond_inline_fn test_cond_inline;

    Address     sp_addr;
    Address     r0_addr;
    Address     r1_addr;
    Address     r2_addr;
    Address     r3_addr;
    Address     lr_addr;
    Address     cy_addr;
    Address     pc_addr;
    set<Address> cpu_regs;
	/* r0-sp */
    set<Address> cpu_base_regs;
    vector<Address *>   argument_regs;

    dobc(const char *slafilename, const char *filename);
    ~dobc();

    void init_regs();
    void init();
    /* 初始化位置位置无关代码，主要时分析原型 */
    void        init_plt(void);

    void        run();
    void        dump_function(char *name);
    void        add_func(funcdata *fd);
    void        set_func_alias(const string &func, const string &alias);
    void        set_test_cond_inline_fn(test_cond_inline_fn fn1) { test_cond_inline = fn1;  }
    funcdata*   find_func(const char *name);
    funcdata*   find_func(const Address &addr);
    funcdata*   find_func_by_alias(const string &name);
    AddrSpace *get_code_space() { return trans->getDefaultCodeSpace();  }
    AddrSpace *get_uniq_space() { return trans->getUniqueSpace();  }
    bool        is_cpu_reg(const Address &addr) { return cpu_regs.find(addr) != cpu_regs.end();  }
    bool        is_cpu_base_reg(const Address &addr) { return cpu_base_regs.find(addr) != cpu_base_regs.end();  }

    void    plugin_dvmp360();

    void gen_sh(void);
    void init_abbrev();
    const string &get_abbrev(const string &name);
};
