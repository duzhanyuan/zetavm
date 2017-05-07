#include <cassert>
#include <iostream>
#include <unordered_map>
#include "runtime.h"
#include "parser.h"
#include "interp.h"
#include "core.h"

/// Inline cache to speed up property lookups
class ICache
{
private:

    // Cached slot index
    size_t slotIdx = 0;

    // Field name to look up
    std::string fieldName;

public:

    ICache(std::string fieldName)
    : fieldName(fieldName)
    {
    }

    Value getField(Object obj)
    {
        Value val;

        if (!obj.getField(fieldName.c_str(), val, slotIdx))
        {
            throw RunError("missing field \"" + fieldName + "\"");
        }

        return val;
    }

    int64_t getInt64(Object obj)
    {
        auto val = getField(obj);
        assert (val.isInt64());
        return (int64_t)val;
    }

    String getStr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isString());
        return String(val);
    }

    Object getObj(Object obj)
    {
        auto val = getField(obj);
        assert (val.isObject());
        return Object(val);
    }

    Array getArr(Object obj)
    {
        auto val = getField(obj);
        assert (val.isArray());
        return Array(val);
    }
};

std::string posToString(Value srcPos)
{
    assert (srcPos.isObject());
    auto srcPosObj = (Object)srcPos;

    auto lineNo = (int64_t)srcPosObj.getField("line_no");
    auto colNo = (int64_t)srcPosObj.getField("col_no");
    auto srcName = (std::string)srcPosObj.getField("src_name");

    return (
        srcName + "@" +
        std::to_string(lineNo) + ":" +
        std::to_string(colNo)
    );
}

/// Opcode enumeration
enum Opcode : uint16_t
{
    GET_LOCAL,
    SET_LOCAL,

    // Stack manipulation
    PUSH,
    POP,
    DUP,
    SWAP,

    // 64-bit integer operations
    ADD_I64,
    SUB_I64,
    MUL_I64,
    LT_I64,
    LE_I64,
    GT_I64,
    GE_I64,
    EQ_I64,

    // Miscellaneous
    EQ_BOOL,
    HAS_TAG,
    GET_TAG,

    // String operations
    STR_LEN,
    GET_CHAR,
    GET_CHAR_CODE,
    STR_CAT,
    EQ_STR,

    // Object operations
    NEW_OBJECT,
    HAS_FIELD,
    SET_FIELD,
    GET_FIELD,
    EQ_OBJ,

    // Array operations
    NEW_ARRAY,
    ARRAY_LEN,
    ARRAY_PUSH,
    GET_ELEM,
    SET_ELEM,

    // Branch instructions
    JUMP,
    JUMP_STUB,
    IF_TRUE,
    CALL,
    RET,

    IMPORT,
    ABORT
};

class CodeFragment
{
public:

    /// Start index in the executable heap
    uint8_t* startPtr = nullptr;

    /// End index in the executable heap
    uint8_t* endPtr = nullptr;

    /// Get the length of the code fragment
    size_t length()
    {
        assert (startPtr);
        assert (endPtr);
        return endPtr - startPtr;
    }
};

class BlockVersion : public CodeFragment
{
public:

    /// Associated block
    Object block;

    /// Code generation context at block entry
    //CodeGenCtx ctx;

    BlockVersion(Object block)
    : block(block)
    {
    }
};

/// Initial code heap size in bytes
const size_t CODE_HEAP_INIT_SIZE = 1 << 20;

/// Initial stack size in words
const size_t STACK_INIT_SIZE = 1 << 16;

/// Flat array of bytes into which code gets compiled
uint8_t* codeHeap = nullptr;

/// Limit pointer for the code heap
uint8_t* codeHeapLimit = nullptr;

/// Current allocation pointer in the code heap
uint8_t* codeHeapAlloc = nullptr;

typedef std::vector<BlockVersion*> VersionList;

/// Map of block objects to lists of versions
std::unordered_map<refptr, VersionList> versionMap;

/// Lower stack limit (stack pointer must be greater than this)
Value* stackLimit = nullptr;

/// Stack base, initial stack pointer value (end of the stack memory array)
Value* stackBase = nullptr;

/// Stack frame base pointer
Value* framePtr = nullptr;

/// Current temp stack top pointer
Value* stackPtr = nullptr;

// Current instruction pointer
uint8_t* instrPtr = nullptr;

/// Cache of all possible one-character string values
Value charStrings[256];

/// Write a value to the code heap
template <typename T> void writeCode(T val)
{
    assert (codeHeapAlloc < codeHeapLimit);
    T* heapPtr = (T*)codeHeapAlloc;
    *heapPtr = val;
    codeHeapAlloc += sizeof(T);
    assert (codeHeapAlloc <= codeHeapLimit);
}

/// Return a pointer to a value to read from the code stream
template <typename T> __attribute__((always_inline)) T& readCode()
{
    assert (instrPtr + sizeof(T) <= codeHeapLimit);
    T* valPtr = (T*)instrPtr;
    instrPtr += sizeof(T);
    return *valPtr;
}

/// Push a value on the stack
__attribute__((always_inline)) void pushVal(Value val)
{
    assert (stackPtr > stackLimit);
    stackPtr--;
    stackPtr[0] = val;
}

/// Push a boolean on the stack
__attribute__((always_inline)) void pushBool(bool val)
{
    pushVal(val? (Value::TRUE) : (Value::FALSE));
}

__attribute__((always_inline)) Value popVal()
{
    assert (stackPtr < stackBase);
    auto val = stackPtr[0];
    stackPtr++;
    return val;
}

__attribute__((always_inline)) bool popBool()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isBool());
    return (bool)val;
}

__attribute__((always_inline)) int64_t popInt64()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isInt64());
    return (int64_t)val;
}

__attribute__((always_inline)) String popStr()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isString());
    return (String)val;
}

__attribute__((always_inline)) Object popObj()
{
    // TODO: throw RunError if wrong type
    auto val = popVal();
    assert (val.isObject());
    return (Object)val;
}

/// Compute the stack size (number of slots allocated)
size_t stackSize()
{
    return stackBase - stackPtr;
}

/// Initialize the interpreter
void initInterp()
{
    // Allocate the code heap
    codeHeap = new uint8_t[CODE_HEAP_INIT_SIZE];
    codeHeapLimit = codeHeap + CODE_HEAP_INIT_SIZE;
    codeHeapAlloc = codeHeap;

    // Allocate the stack
    stackLimit = new Value[STACK_INIT_SIZE];
    stackBase = stackLimit + STACK_INIT_SIZE;
    stackPtr = stackBase;
}

/// Get a version of a block. This version will be a stub
/// until compiled
BlockVersion* getBlockVersion(Object block)
{
    auto blockPtr = (refptr)block;

    auto versionItr = versionMap.find((refptr)block);

    if (versionItr == versionMap.end())
    {
        versionMap[blockPtr] = VersionList();
    }
    else
    {
        auto versions = versionItr->second;
        assert (versions.size() == 1);
        return versions[0];
    }

    auto newVersion = new BlockVersion(block);

    auto& versionList = versionMap[blockPtr];
    versionList.push_back(newVersion);

    return newVersion;
}

void compile(BlockVersion* version)
{
    //std::cout << "compiling version" << std::endl;

    auto block = version->block;

    // Get the instructions array
    static ICache instrsIC("instrs");
    Array instrs = instrsIC.getArr(block);

    if (instrs.length() == 0)
    {
        throw RunError("empty basic block");
    }

    // Mark the block start
    version->startPtr = codeHeapAlloc;

    // For each instruction
    for (size_t i = 0; i < instrs.length(); ++i)
    {
        auto instrVal = instrs.getElem(i);
        assert (instrVal.isObject());
        auto instr = (Object)instrVal;

        static ICache opIC("op");
        auto op = (std::string)opIC.getStr(instr);

        //std::cout << "op: " << op << std::endl;

        if (op == "push")
        {
            static ICache valIC("val");
            auto val = valIC.getField(instr);
            writeCode(PUSH);
            writeCode(val);
            continue;
        }

        if (op == "pop")
        {
            writeCode(POP);
            continue;
        }

        if (op == "dup")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt64(instr);
            writeCode(DUP);
            writeCode(idx);
            continue;
        }

        if (op == "swap")
        {
            writeCode(SWAP);
            continue;
        }

        if (op == "get_local")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt64(instr);
            writeCode(GET_LOCAL);
            writeCode(idx);
            continue;
        }

        if (op == "set_local")
        {
            static ICache idxIC("idx");
            auto idx = (uint16_t)idxIC.getInt64(instr);
            writeCode(SET_LOCAL);
            writeCode(idx);
            continue;
        }

        //
        // Integer operations
        //

        if (op == "add_i64")
        {
            writeCode(ADD_I64);
            continue;
        }

        if (op == "sub_i64")
        {
            writeCode(SUB_I64);
            continue;
        }

        if (op == "mul_i64")
        {
            writeCode(MUL_I64);
            continue;
        }

        if (op == "lt_i64")
        {
            writeCode(LT_I64);
            continue;
        }

        if (op == "le_i64")
        {
            writeCode(LE_I64);
            continue;
        }

        if (op == "gt_i64")
        {
            writeCode(GT_I64);
            continue;
        }

        if (op == "ge_i64")
        {
            writeCode(GE_I64);
            continue;
        }

        if (op == "eq_i64")
        {
            writeCode(EQ_I64);
            continue;
        }

        //
        // Miscellaneous ops
        //

        if (op == "eq_bool")
        {
            writeCode(EQ_BOOL);
            continue;
        }

        if (op == "has_tag")
        {
            static ICache tagIC("tag");
            auto tagStr = (std::string)tagIC.getStr(instr);
            auto tag = strToTag(tagStr);

            writeCode(HAS_TAG);
            writeCode(tag);
            continue;
        }

        //
        // String operations
        //

        if (op == "str_len")
        {
            writeCode(STR_LEN);
            continue;
        }

        if (op == "get_char")
        {
            writeCode(GET_CHAR);
            continue;
        }

        if (op == "get_char_code")
        {
            writeCode(GET_CHAR_CODE);
            continue;
        }

        if (op == "str_cat")
        {
            writeCode(STR_CAT);
            continue;
        }

        if (op == "eq_str")
        {
            writeCode(EQ_STR);
            continue;
        }

        //
        // Object operations
        //

        if (op == "new_object")
        {
            writeCode(NEW_OBJECT);
            continue;
        }

        if (op == "has_field")
        {
            writeCode(HAS_FIELD);
            continue;
        }

        if (op == "set_field")
        {
            writeCode(SET_FIELD);
            continue;
        }

        if (op == "get_field")
        {
            writeCode(GET_FIELD);
            continue;
        }

        //
        // Array operations
        //

        if (op == "new_array")
        {
            writeCode(NEW_ARRAY);
            continue;
        }

        if (op == "array_len")
        {
            writeCode(ARRAY_LEN);
            continue;
        }

        if (op == "array_push")
        {
            writeCode(ARRAY_PUSH);
            continue;
        }

        if (op == "set_elem")
        {
            writeCode(SET_ELEM);
            continue;
        }

        if (op == "get_elem")
        {
            writeCode(GET_ELEM);
            continue;
        }

        if (op == "eq_obj")
        {
            writeCode(EQ_OBJ);
            continue;
        }

        //
        // Branch instructions
        //

        if (op == "jump")
        {
            static ICache toIC("to");
            auto dstBB = toIC.getObj(instr);

            auto dstVer = getBlockVersion(dstBB);

            writeCode(JUMP_STUB);
            writeCode(dstVer);
            continue;
        }

        if (op == "if_true")
        {
            static ICache thenIC("then");
            static ICache elseIC("else");
            auto thenBB = thenIC.getObj(instr);
            auto elseBB = elseIC.getObj(instr);

            auto thenVer = getBlockVersion(thenBB);
            auto elseVer = getBlockVersion(elseBB);

            writeCode(IF_TRUE);
            writeCode(thenVer);
            writeCode(elseVer);

            continue;
        }

        if (op == "call")
        {
            static ICache retToCache("ret_to");
            static ICache numArgsCache("num_args");
            auto numArgs = (int16_t)numArgsCache.getInt64(instr);

            // Get a version for the call continuation block
            auto retToBB = retToCache.getObj(instr);
            auto retVer = getBlockVersion(retToBB);

            writeCode(CALL);
            writeCode(numArgs);
            writeCode(retVer);

            continue;
        }

        if (op == "ret")
        {
            writeCode(RET);
            continue;
        }

        if (op == "import")
        {
            writeCode(IMPORT);
            continue;
        }

        if (op == "abort")
        {
            writeCode(ABORT);
            continue;
        }

        throw RunError("unhandled opcode in basic block \"" + op + "\"");
    }

    // Mark the block end
    version->endPtr = codeHeapAlloc;
}

// TODO: wrap into function
/*
    if (numArgs != numParams)
    {
        std::string srcPosStr = (
            instr.hasField("src_pos")?
            (posToString(instr.getField("src_pos")) + " - "):
            std::string("")
        );

        throw RunError(
            srcPosStr +
            "incorrect argument count in call, received " +
            std::to_string(numArgs) +
            ", expected " +
            std::to_string(numParams)
        );
    }
*/

/// Start/continue execution beginning at a current instruction
Value execCode()
{
    assert (instrPtr >= codeHeap);
    assert (instrPtr < codeHeapLimit);

    // For each instruction to execute
    for (;;)
    {
        auto& op = readCode<Opcode>();

        //std::cout << "instr" << std::endl;
        //std::cout << "op=" << (int)op << std::endl;
        //std::cout << "  stack space: " << (stackBase - stackPtr) << std::endl;

        switch (op)
        {
            case PUSH:
            {
                auto val = readCode<Value>();
                pushVal(val);
            }
            break;

            case POP:
            {
                popVal();
            }
            break;

            case DUP:
            {
                // Read the index of the value to duplicate
                auto idx = readCode<uint16_t>();
                auto val = stackPtr[idx];
                pushVal(val);
            }
            break;

            // Swap the topmost two stack elements
            case SWAP:
            {
                auto v0 = popVal();
                auto v1 = popVal();
                pushVal(v0);
                pushVal(v1);
            }
            break;

            // Set a local variable
            case SET_LOCAL:
            {
                auto localIdx = readCode<uint16_t>();
                //std::cout << "set localIdx=" << localIdx << std::endl;
                assert (stackPtr > stackLimit);
                framePtr[-localIdx] = popVal();
            }
            break;

            case GET_LOCAL:
            {
                // Read the index of the value to push
                auto localIdx = readCode<uint16_t>();
                //std::cout << "get localIdx=" << localIdx << std::endl;
                assert (stackPtr > stackLimit);
                auto val = framePtr[-localIdx];
                pushVal(val);
            }
            break;

            //
            // Integer operations
            //

            case ADD_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushVal((int64_t)arg0 + (int64_t)arg1);
            }
            break;

            case SUB_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushVal((int64_t)arg0 - (int64_t)arg1);
            }
            break;

            case MUL_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushVal((int64_t)arg0 * (int64_t)arg1);
            }
            break;

            case LT_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushBool((int64_t)arg0 < (int64_t)arg1);
            }
            break;

            case LE_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushBool((int64_t)arg0 <= (int64_t)arg1);
            }
            break;

            case GT_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushBool((int64_t)arg0 > (int64_t)arg1);
            }
            break;

            case GE_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushBool((int64_t)arg0 >= (int64_t)arg1);
            }
            break;

            case EQ_I64:
            {
                auto arg1 = popVal();
                auto arg0 = popVal();
                pushBool((int64_t)arg0 == (int64_t)arg1);
            }
            break;

            //
            // Misc operations
            //

            case EQ_BOOL:
            {
                auto arg1 = popBool();
                auto arg0 = popBool();
                pushBool(arg0 == arg1);
            }
            break;

            // Test if a value has a given tag
            case HAS_TAG:
            {
                auto testTag = readCode<Tag>();
                auto valTag = popVal().getTag();
                pushBool(valTag == testTag);
            }
            break;

            //
            // String operations
            //

            case STR_LEN:
            {
                auto str = popStr();
                pushVal(str.length());
            }
            break;

            case GET_CHAR:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char, index out of bounds"
                    );
                }

                auto ch = str[idx];

                // Cache single-character strings
                if (charStrings[ch] == Value::FALSE)
                {
                    char buf[2] = { (char)str[idx], '\0' };
                    charStrings[ch] = String(buf);
                }

                pushVal(charStrings[ch]);
            }
            break;

            case GET_CHAR_CODE:
            {
                auto idx = (size_t)popInt64();
                auto str = popStr();

                if (idx >= str.length())
                {
                    throw RunError(
                        "get_char_code, index out of bounds"
                    );
                }

                pushVal((int64_t)str[idx]);
            }
            break;

            case STR_CAT:
            {
                auto a = popStr();
                auto b = popStr();
                auto c = String::concat(b, a);
                pushVal(c);
            }
            break;

            case EQ_STR:
            {
                auto arg1 = popStr();
                auto arg0 = popStr();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Object operations
            //

            case NEW_OBJECT:
            {
                auto capacity = popInt64();
                auto obj = Object::newObject(capacity);
                pushVal(obj);
            }
            break;

            case HAS_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();
                pushBool(obj.hasField(fieldName));
            }
            break;

            case SET_FIELD:
            {
                auto val = popVal();
                auto fieldName = popStr();
                auto obj = popObj();

                if (!isValidIdent(fieldName))
                {
                    throw RunError(
                        "invalid identifier in set_field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                obj.setField(fieldName, val);
            }
            break;

            // This instruction will abort execution if trying to
            // access a field that is not present on an object.
            // The running program is responsible for testing that
            // fields exist before attempting to read them.
            case GET_FIELD:
            {
                auto fieldName = popStr();
                auto obj = popObj();

                if (!obj.hasField(fieldName))
                {
                    throw RunError(
                        "get_field failed, missing field \"" +
                        (std::string)fieldName + "\""
                    );
                }

                auto val = obj.getField(fieldName);
                pushVal(val);
            }
            break;

            case EQ_OBJ:
            {
                Value arg1 = popVal();
                Value arg0 = popVal();
                pushBool(arg0 == arg1);
            }
            break;

            //
            // Array operations
            //

            case NEW_ARRAY:
            {
                auto len = popInt64();
                auto array = Array(len);
                pushVal(array);
            }
            break;

            case ARRAY_LEN:
            {
                auto arr = Array(popVal());
                pushVal(arr.length());
            }
            break;

            case ARRAY_PUSH:
            {
                auto val = popVal();
                auto arr = Array(popVal());
                arr.push(val);
            }
            break;

            case SET_ELEM:
            {
                auto val = popVal();
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "set_elem, index out of bounds"
                    );
                }

                arr.setElem(idx, val);
            }
            break;

            case GET_ELEM:
            {
                auto idx = (size_t)popInt64();
                auto arr = Array(popVal());

                if (idx >= arr.length())
                {
                    throw RunError(
                        "get_elem, index out of bounds"
                    );
                }

                pushVal(arr.getElem(idx));
            }
            break;

            //
            // Branch instructions
            //

            case JUMP_STUB:
            {
                auto& dstAddr = readCode<uint8_t*>();

                //std::cout << "Patching jump" << std::endl;

                auto dstVer = (BlockVersion*)dstAddr;

                if (!dstVer->startPtr)
                    compile(dstVer);

                // Patch the jump
                op = JUMP;
                dstAddr = dstVer->startPtr;

                // Jump to the target
                instrPtr = dstVer->startPtr;
            }
            break;

            case JUMP:
            {
                auto& dstAddr = readCode<uint8_t*>();
                instrPtr = dstAddr;
            }
            break;

            case IF_TRUE:
            {
                auto& thenAddr = readCode<uint8_t*>();
                auto& elseAddr = readCode<uint8_t*>();

                auto arg0 = popVal();

                if (arg0 == Value::TRUE)
                {
                    if (thenAddr < codeHeap || thenAddr >= codeHeapLimit)
                    {
                        //std::cout << "Patching then target" << std::endl;

                        auto thenVer = (BlockVersion*)thenAddr;
                        if (!thenVer->startPtr)
                           compile(thenVer);

                        // Patch the jump
                        thenAddr = thenVer->startPtr;
                    }

                    instrPtr = thenAddr;
                }
                else
                {
                    if (elseAddr < codeHeap || elseAddr >= codeHeapLimit)
                    {
                       //std::cout << "Patching else target" << std::endl;

                       auto elseVer = (BlockVersion*)elseAddr;
                       if (!elseVer->startPtr)
                           compile(elseVer);

                       // Patch the jump
                       elseAddr = elseVer->startPtr;
                    }

                    instrPtr = elseAddr;
                }
            }
            break;

            // Regular function call
            case CALL:
            {
                auto numArgs = readCode<uint16_t>();
                auto retVer = readCode<BlockVersion*>();

                auto callee = popVal();

                //std::cout << "call, numArgs=" << numArgs << std::endl;

                if (stackSize() < numArgs)
                {
                    throw RunError(
                        "stack underflow at call"
                    );
                }

                if (callee.isObject())
                {
                    // TODO: we could inline cache some function
                    // information
                    // start with map of fn objs to structs
                    // TODO: move callFn into its own function

                    // Get a version for the function entry block
                    static ICache entryIC("entry");
                    auto entryBB = entryIC.getObj(callee);
                    auto entryVer = getBlockVersion(entryBB);

                    if (!entryVer->startPtr)
                    {
                        //std::cout << "compiling function entry block" << std::endl;
                        compile(entryVer);
                    }

                    static ICache localsIC("num_locals");
                    auto numLocals = localsIC.getInt64(callee);

                    static ICache paramsIC("num_params");
                    auto numParams = paramsIC.getInt64(callee);

                    if (numLocals < numParams)
                    {
                        throw RunError(
                            "not enough locals to store function parameters"
                        );
                    }

                    if (numArgs != numParams)
                    {
                        throw RunError("argument count mismatch");
                    }

                    // Compute the stack pointer to restore after the call
                    auto prevStackPtr = stackPtr + numArgs;

                    // Save the current frame pointer
                    auto prevFramePtr = framePtr;

                    // Point the frame pointer to the first argument
                    assert (stackPtr > stackLimit);
                    framePtr = stackPtr + numArgs - 1;

                    // Pop the arguments, push the callee locals
                    stackPtr -= numLocals - numArgs;

                    pushVal(Value((refptr)prevStackPtr, TAG_RAWPTR));
                    pushVal(Value((refptr)prevFramePtr, TAG_RAWPTR));
                    pushVal(Value((refptr)retVer, TAG_RAWPTR));

                    // Jump to the entry block of the function
                    instrPtr = entryVer->startPtr;
                }
                else if (callee.isHostFn())
                {
                    auto hostFn = (HostFn*)callee.getWord().ptr;

                    // Pointer to the first argument
                    auto args = stackPtr + numArgs - 1;

                    Value retVal;

                    // Call the host function
                    switch (numArgs)
                    {
                        case 0:
                        retVal = hostFn->call0();
                        break;

                        case 1:
                        retVal = hostFn->call1(args[0]);
                        break;

                        case 2:
                        retVal = hostFn->call2(args[0], args[1]);
                        break;

                        case 3:
                        retVal = hostFn->call3(args[0], args[1], args[2]);
                        break;

                        default:
                        assert (false);
                    }

                    // Pop the arguments from the stack
                    stackPtr += numArgs;

                    // Push the return value
                    pushVal(retVal);

                    if (!retVer->startPtr)
                        compile(retVer);

                    instrPtr = retVer->startPtr;
                }
                else
                {
                  throw RunError("invalid callee at call site");
                }
            }
            break;

            case RET:
            {
                // TODO: figure out callee identity from version,
                // caller identity from return address
                //
                // We want args to have been consumed
                // We pop all our locals (or the caller does)
                //
                // The thing is... The caller can't pop our locals,
                // because the call continuation doesn't know

                // Pop the return value
                auto retVal = popVal();

                // Pop the return address
                auto retVer = (BlockVersion*)popVal().getWord().ptr;

                // Pop the previous frame pointer
                auto prevFramePtr = popVal().getWord().ptr;

                // Pop the previous stack pointer
                auto prevStackPtr = popVal().getWord().ptr;

                // Restore the previous frame pointer
                framePtr = (Value*)prevFramePtr;

                // Restore the stack pointer
                stackPtr = (Value*)prevStackPtr;

                // If this is a top-level return
                if (retVer == nullptr)
                {
                    return retVal;
                }
                else
                {
                    // Push the return value on the stack
                    pushVal(retVal);

                    if (!retVer->startPtr)
                        compile(retVer);

                    instrPtr = retVer->startPtr;
                }
            }
            break;

            case IMPORT:
            {
                auto pkgName = (std::string)popVal();
                auto pkg = import(pkgName);
                pushVal(pkg);
            }
            break;

            case ABORT:
            {
                auto errMsg = (std::string)popStr();

                // FIXME
                /*
                // If a source position was specified
                if (instr.hasField("src_pos"))
                {
                    auto srcPos = instr.getField("src_pos");
                    std::cout << posToString(srcPos) << " - ";
                }
                */

                if (errMsg != "")
                {
                    std::cout << "aborting execution due to error: ";
                    std::cout << errMsg << std::endl;
                }
                else
                {
                    std::cout << "aborting execution due to error" << std::endl;
                }

                exit(-1);
            }
            break;

            default:
            assert (false && "unhandled instruction in interpreter loop");
        }

    }

    assert (false);
}

/// Begin the execution of a function
/// Note: this may be indirectly called from within a running interpreter
Value callFun(Object fun, ValueVec args)
{
    static ICache numParamsIC("num_params");
    static ICache numLocalsIC("num_locals");
    auto numParams = numParamsIC.getInt64(fun);
    auto numLocals = numLocalsIC.getInt64(fun);
    assert (args.size() <= numParams);
    assert (numParams <= numLocals);

    // Store the stack size before the call
    auto preCallSz = stackSize();

    // Save the previous instruction pointer
    pushVal(Value((refptr)instrPtr, TAG_RAWPTR));

    // Save the previous stack and frame pointers
    auto prevStackPtr = stackPtr;
    auto prevFramePtr = framePtr;

    // Initialize the frame pointer (used to access locals)
    framePtr = stackPtr - 1;

    // Push space for the local variables
    stackPtr -= numLocals;
    assert (stackPtr >= stackLimit);

    // Push the previous stack pointer, previous
    // frame pointer and return address
    pushVal(Value((refptr)prevStackPtr, TAG_RAWPTR));
    pushVal(Value((refptr)prevFramePtr, TAG_RAWPTR));
    pushVal(Value(nullptr, TAG_RAWPTR));

    // Copy the arguments into the locals
    for (size_t i = 0; i < args.size(); ++i)
    {
        //std::cout << "  " << args[i].toString() << std::endl;
        framePtr[-i] = args[i];
    }

    // Get the function entry block
    static ICache entryIC("entry");
    auto entryBlock = entryIC.getObj(fun);
    auto entryVer = getBlockVersion(entryBlock);

    // Generate code for the entry block version
    compile(entryVer);
    assert (entryVer->length() > 0);

    // Begin execution at the entry block
    instrPtr = entryVer->startPtr;
    auto retVal = execCode();

    // Restore the previous instruction pointer
    instrPtr = (uint8_t*)popVal().getWord().ptr;

    // Check that the stack size matches what it was before the call
    assert (stackSize() == preCallSz);

    return retVal;
}

/// Call a function exported by a package
Value callExportFn(
    Object pkg,
    std::string fnName,
    ValueVec args
)
{
    assert (pkg.hasField(fnName));
    auto fnVal = pkg.getField(fnName);
    assert (fnVal.isObject());
    auto funObj = Object(fnVal);

    return callFun(funObj, args);
}

Value testRunImage(std::string fileName)
{
    std::cout << "loading image \"" << fileName << "\"" << std::endl;

    auto pkg = parseFile(fileName);

    return callExportFn(pkg, "main");
}

void testInterp()
{
    assert (testRunImage("tests/vm/ex_ret_cst.zim") == Value(777));
    assert (testRunImage("tests/vm/ex_loop_cnt.zim") == Value(0));
    //assert (testRunImage("tests/vm/ex_image.zim") == Value(10));
    assert (testRunImage("tests/vm/ex_rec_fact.zim") == Value(5040));
    assert (testRunImage("tests/vm/ex_fibonacci.zim") == Value(377));
}
