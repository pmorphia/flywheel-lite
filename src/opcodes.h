/* Bytecode instruction set. Operands are big-endian u16 unless noted. */
#ifndef LITE_OPCODES_H
#define LITE_OPCODES_H

#define OPCODES(X)                                                                                \
    X(OP_CONST)          /* u16 const */                                                          \
    X(OP_NULL)                                                                                    \
    X(OP_TRUE)                                                                                    \
    X(OP_FALSE)                                                                                   \
    X(OP_INT)            /* s16 */                                                                \
    X(OP_POP)                                                                                     \
    X(OP_DUP)                                                                                     \
    X(OP_DUP2)                                                                                    \
    X(OP_SWAP)                                                                                    \
    X(OP_GET_LOCAL)      /* u8 */                                                                 \
    X(OP_SET_LOCAL)      /* u8, pops */                                                           \
    X(OP_GET_UPVAL)      /* u8 */                                                                 \
    X(OP_SET_UPVAL)      /* u8, pops */                                                           \
    X(OP_GET_GLOBAL)     /* u16 slot */                                                           \
    X(OP_SET_GLOBAL)     /* u16 slot, pops */                                                     \
    X(OP_GET_MGLOBAL)    /* u16 module const, u16 slot */                                         \
    X(OP_SET_MGLOBAL)    /* u16 module const, u16 slot, pops */                                   \
    X(OP_GET_MEMBER)     /* u16 name, u16 cache */                                                \
    X(OP_SET_MEMBER)     /* u16 name, u16 cache: [obj value] -> [] */                             \
    X(OP_GET_INDEX)                                                                               \
    X(OP_SLICE)          /* u8 inclusive: [obj lo hi] -> [slice] */                               \
    X(OP_FIELD_UNSET)    /* u16 name: [obj] -> [bool] */                                          \
    X(OP_SET_INDEX)      /* [obj key value] -> [] */                                              \
    X(OP_ADD)                                                                                     \
    X(OP_SUB)                                                                                     \
    X(OP_MUL)                                                                                     \
    X(OP_DIV)                                                                                     \
    X(OP_IDIV)                                                                                    \
    X(OP_MOD)                                                                                     \
    X(OP_NEG)                                                                                     \
    X(OP_POS)                                                                                     \
    X(OP_NOT)                                                                                     \
    X(OP_EQ)                                                                                      \
    X(OP_NE)                                                                                      \
    X(OP_LT)                                                                                      \
    X(OP_LE)                                                                                      \
    X(OP_GT)                                                                                      \
    X(OP_GE)                                                                                      \
    X(OP_IN)                                                                                      \
    X(OP_CHAIN)          /* u8 comparison opcode: [a b] -> [b bool] */                            \
    X(OP_RANGE)          /* u8 inclusive */                                                       \
    X(OP_IS)             /* u16 type */                                                           \
    X(OP_AS)             /* u16 type */                                                           \
    X(OP_COERCE)         /* u16 type, u16 what-const */                                           \
    X(OP_COERCE_LOCAL)   /* u8 slot, u16 type, u16 what-const */                                  \
    X(OP_JUMP)           /* u16 forward */                                                        \
    X(OP_JUMP_IF_FALSE)  /* u16, pops */                                                          \
    X(OP_AND)            /* u16: false keeps and jumps, else pops */                              \
    X(OP_OR)             /* u16: true keeps and jumps, else pops */                               \
    X(OP_JUMP_NOT_NULL)  /* u16: non-null keeps and jumps, else pops */                           \
    X(OP_LOOP)           /* u16 backward */                                                       \
    X(OP_CALL)           /* u8 argc */                                                            \
    X(OP_INVOKE)         /* u16 name, u8 argc, u16 cache */                                       \
    X(OP_SUPER_INVOKE)   /* u16 name, u8 argc */                                                  \
    X(OP_SUPER_GET)      /* u16 name */                                                           \
    X(OP_CALL_STMT)      /* call with no arguments when callable */                               \
    X(OP_CLOSURE)        /* u16 fn, then (u8 is_local, u8 index) per upvalue */                   \
    X(OP_CLOSE_UPVAL)                                                                             \
    X(OP_RETURN)                                                                                  \
    X(OP_ARRAY)          /* u16 n: array, or tuple when heterogeneous */                          \
    X(OP_MAP)            /* u16 pairs */                                                          \
    X(OP_SET)            /* u16 n */                                                              \
    X(OP_STRING)         /* u16 n: concatenate formatted parts */                                 \
    X(OP_FORMAT)         /* u16 spec */                                                           \
    X(OP_ELEMENT)        /* u16 tag (or null), u16 attribute names, u16 children */               \
    X(OP_JSON)           /* u16 text const */                                                     \
    X(OP_TRY)            /* u16 handler offset */                                                 \
    X(OP_POP_HANDLER)                                                                             \
    X(OP_THROW)                                                                                   \
    X(OP_THROW_NAMED)    /* [name message] */                                                     \
    X(OP_THROW_WITH)     /* [subject message]: throw X with "message" */                          \
    X(OP_CATCH_MATCH)    /* u16 type-name const: [err] -> [err bool] */                           \
    X(OP_FOR_PREP)       /* u8 slot, u8 names */                                                  \
    X(OP_FOR_NEXT)       /* u8 slot, u8 names, u16 exit */                                        \
    X(OP_RANGE_PREP)     /* u8 slot */                                                            \
    X(OP_RANGE_NEXT)     /* u8 slot, u8 inclusive, u16 exit */                                    \
    X(OP_ARG_PRESENT)    /* u8 index, u16 jump when present */                                    \
    X(OP_INHERIT)        /* u16 class: [base] -> [] */                                            \
    X(OP_IMPLEMENT)      /* u16 class: [interface] -> [] */                                       \
    X(OP_FINALIZE)       /* u16 class -> [class] */                                               \
    X(OP_MATCH_VALUE)    /* [subject pattern] -> [bool] */                                        \
    X(OP_MATCH_ENUM)     /* u16 name: [subject fallback] -> [bool] */                             \
    X(OP_IN_RANGE)       /* u8 inclusive: [v lo hi] -> [bool] */                                  \
    X(OP_LEN_IS)         /* u16 n, u8 exact-array: [v] -> [bool] */                               \
    X(OP_FIELD_AT)       /* u8 index: [obj] -> [field] */                                         \
    X(OP_RESULT_IS)      /* u8 ok: [v] -> [bool] */                                               \
    X(OP_RESULT_VALUE)                                                                            \
    X(OP_DESTRUCTURE)    /* u16 n: checks the length of the top value */                          \
    X(OP_ATTR)           /* u16 name: [attrs] -> [value] */                                       \
    X(OP_NO_MATCH)                                                                                \
    X(OP_IMPORT)         /* u16 module const: runs its initializer once */                        \
    X(OP_TYPE_ERROR)     /* u16 message const */

typedef enum {
#define X(name) name,
    OPCODES(X)
#undef X
        OP__COUNT
} OpCode;

#endif
