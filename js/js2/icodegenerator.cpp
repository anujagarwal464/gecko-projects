/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
*
* The contents of this file are subject to the Netscape Public
* License Version 1.1 (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of
* the License at http://www.mozilla.org/NPL/
*
* Software distributed under the License is distributed on an "AS
* IS" basis, WITHOUT WARRANTY OF ANY KIND, either express oqr
* implied. See the License for the specific language governing
* rights and limitations under the License.
*
* The Original Code is the JavaScript 2 Prototype.
*
* The Initial Developer of the Original Code is Netscape
* Communications Corporation.  Portions created by Netscape are
* Copyright (C) 1998 Netscape Communications Corporation. All
* Rights Reserved.
*
* Contributor(s): 
*
* Alternatively, the contents of this file may be used under the
* terms of the GNU Public License (the "GPL"), in which case the
* provisions of the GPL are applicable instead of those above.
* If you wish to allow use of your version of this file only
* under the terms of the GPL and not to allow others to use your
* version of this file under the NPL, indicate your decision by
* deleting the provisions above and replace them with the notice
* and other provisions required by the GPL.  If you do not delete
* the provisions above, a recipient may use your version of this
* file under either the NPL or the GPL.
*/

#include "numerics.h"
#include "world.h"
#include "vmtypes.h"
#include "jstypes.h"
#include "icodegenerator.h"

#include <stdexcept>

namespace JavaScript {
namespace ICG {

using namespace VM;
using namespace JSTypes;

uint32 ICodeModule::sMaxID = 0;
    
Formatter& operator<<(Formatter &f, ICodeGenerator &i)
{
    return i.print(f);
}

Formatter& operator<<(Formatter &f, ICodeModule &i)
{
    return i.print(f);
}

//
// ICodeGenerator
//

ICodeGenerator::ICodeGenerator(World *world)
    :   topRegister(0), 
        registerBase(0), 
        maxRegister(0), 
        parameterCount(0),
        exceptionRegister(NotARegister),
        variableList(new VariableList()),
        mWorld(world),
        mInstructionMap(new InstructionMap()),
        mWithinWith(false)

{ 
    iCode = new InstructionStream();
    iCodeOwner = true;
}

Register ICodeGenerator::allocateVariable(const StringAtom& name) 
{ 
    if (exceptionRegister == NotARegister) {
        exceptionRegister = grabRegister(mWorld->identifiers[widenCString("__exceptionObject__")]);
    }
    return grabRegister(name); 
}

ICodeModule *ICodeGenerator::complete()
{
#ifdef DEBUG
    for (LabelList::iterator i = labels.begin();
         i != labels.end(); i++) {
        ASSERT((*i)->mBase == iCode);
        ASSERT((*i)->mOffset <= iCode->size());
    }
#endif
    /*
    XXX FIXME
    I wanted to do the following rather than have to have the label set hanging around as well
    as the ICodeModule. Branches have since changed, but the concept is still good and should
    be re-introduced at some point.

    for (InstructionIterator ii = iCode->begin(); 
         ii != iCode->end(); ii++) {            
        if ((*ii)->op() == BRANCH) {
            Instruction *t = *ii;
            *ii = new ResolvedBranch(static_cast<Branch *>(*ii)->operand1->itsOffset);
            delete t;
        }
        else 
            if ((*ii)->itsOp >= BRANCH_LT && (*ii)->itsOp <= BRANCH_GT) {
                Instruction *t = *ii;
                *ii = new ResolvedBranchCond((*ii)->itsOp, 
                                             static_cast<BranchCond *>(*ii)->itsOperand1->itsOffset,
                                             static_cast<BranchCond *>(*ii)->itsOperand2);
                delete t;
            }
    }
    */
    markMaxRegister();
    ICodeModule* module = new ICodeModule(iCode, variableList, maxRegister, 0, mInstructionMap);
    iCodeOwner = false;   // give ownership to the module.
    return module;
}


/********************************************************************/

Register ICodeGenerator::loadImmediate(double value)
{
    Register dest = getRegister();
    LoadImmediate *instr = new LoadImmediate(dest, value);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::loadString(String &value)
{
    Register dest = getRegister();
    LoadString *instr = new LoadString(dest, new JSString(value));
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::loadValue(JSValue value)
{
    Register dest = getRegister();
    LoadValue *instr = new LoadValue(dest, value);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::newObject()
{
    Register dest = getRegister();
    NewObject *instr = new NewObject(dest);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::newArray()
{
    Register dest = getRegister();
    NewArray *instr = new NewArray(dest);
    iCode->push_back(instr);
    return dest;
}



Register ICodeGenerator::loadName(const StringAtom &name)
{
    Register dest = getRegister();
    LoadName *instr = new LoadName(dest, &name);
    iCode->push_back(instr);
    return dest;
}

void ICodeGenerator::saveName(const StringAtom &name, Register value)
{
    SaveName *instr = new SaveName(&name, value);
    iCode->push_back(instr);
}

Register ICodeGenerator::nameInc(const StringAtom &name)
{
    Register dest = getRegister();
    NameXcr *instr = new NameXcr(dest, &name, 1.0);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::nameDec(const StringAtom &name)
{
    Register dest = getRegister();
    NameXcr *instr = new NameXcr(dest, &name, -1.0);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::varInc(Register var)
{
    Register dest = getRegister();
    VarXcr *instr = new VarXcr(dest, var, 1.0);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::varDec(Register var)
{
    Register dest = getRegister();
    VarXcr *instr = new VarXcr(dest, var, -1.0);
    iCode->push_back(instr);
    return dest;
}



Register ICodeGenerator::getProperty(Register base, const StringAtom &name)
{
    Register dest = getRegister();
    GetProp *instr = new GetProp(dest, base, &name);
    iCode->push_back(instr);
    return dest;
}

void ICodeGenerator::setProperty(Register base, const StringAtom &name,
                                 Register value)
{
    SetProp *instr = new SetProp(base, &name, value);
    iCode->push_back(instr);
}

Register ICodeGenerator::propertyInc(Register base, const StringAtom &name)
{
    Register dest = getRegister();
    PropXcr *instr = new PropXcr(dest, base, &name, 1.0);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::propertyDec(Register base, const StringAtom &name)
{
    Register dest = getRegister();
    PropXcr *instr = new PropXcr(dest, base, &name, -1.0);
    iCode->push_back(instr);
    return dest;
}



Register ICodeGenerator::getElement(Register base, Register index)
{
    Register dest = getRegister();
    GetElement *instr = new GetElement(dest, base, index);
    iCode->push_back(instr);
    return dest;
}

void ICodeGenerator::setElement(Register base, Register index,
                                Register value)
{
    SetElement *instr = new SetElement(base, index, value);
    iCode->push_back(instr);
}

Register ICodeGenerator::elementInc(Register base, Register index)
{
    Register dest = getRegister();
    ElemXcr *instr = new ElemXcr(dest, base, index, 1.0);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::elementDec(Register base, Register index)
{
    Register dest = getRegister();
    ElemXcr *instr = new ElemXcr(dest, base, index, -1.0);
    iCode->push_back(instr);
    return dest;
}


Register ICodeGenerator::op(ICodeOp op, Register source)
{
    Register dest = getRegister();
    ASSERT(source != NotARegister);    
    Unary *instr = new Unary (op, dest, source);
    iCode->push_back(instr);
    return dest;
}

    
void ICodeGenerator::move(Register destination, Register source)
{
    ASSERT(destination != NotARegister);    
    ASSERT(source != NotARegister);    
    Move *instr = new Move(destination, source);
    iCode->push_back(instr);
}

Register ICodeGenerator::logicalNot(Register source)
{
    Register dest = getRegister();
    Not *instr = new Not(dest, source);
    iCode->push_back(instr);
    return dest;
} 

Register ICodeGenerator::test(Register source)
{
    Register dest = getRegister();
    Test *instr = new Test(dest, source);
    iCode->push_back(instr);
    return dest;
}

Register ICodeGenerator::op(ICodeOp op, Register source1, 
                            Register source2)
{
    ASSERT(source1 != NotARegister);    
    ASSERT(source2 != NotARegister);    
    Register dest = getRegister();
    Arithmetic *instr = new Arithmetic(op, dest, source1, source2);
    iCode->push_back(instr);
    return dest;
} 
    
Register ICodeGenerator::call(Register target, RegisterList args)
{
    Register dest = getRegister();
    Call *instr = new Call(dest, target, args);
    iCode->push_back(instr);
    return dest;
}

void ICodeGenerator::callVoid(Register target, RegisterList args)
{
    Call *instr = new Call(NotARegister, target, args);
    iCode->push_back(instr);
}

void ICodeGenerator::branch(Label *label)
{
    Branch *instr = new Branch(label);
    iCode->push_back(instr);
}

GenericBranch *ICodeGenerator::branchTrue(Label *label, Register condition)
{
    GenericBranch *instr = new GenericBranch(BRANCH_TRUE, label, condition);
    iCode->push_back(instr);
    return instr;
}

GenericBranch *ICodeGenerator::branchFalse(Label *label, Register condition)
{
    GenericBranch *instr = new GenericBranch(BRANCH_FALSE, label, condition);
    iCode->push_back(instr);
    return instr;
}

void ICodeGenerator::returnStmt(Register r)
{
    if (r == NotARegister)
        iCode->push_back(new ReturnVoid());
    else
        iCode->push_back(new Return(r));
}


/********************************************************************/

Label *ICodeGenerator::getLabel()
{
    labels.push_back(new Label(NULL));
    return labels.back();
}

void ICodeGenerator::setLabel(Label *l)
{
    l->mBase = iCode;
    l->mOffset = iCode->size();
}

/************************************************************************/




ICodeOp ICodeGenerator::mapExprNodeToICodeOp(ExprNode::Kind kind)
{
    // can be an array later, when everything has settled down
    switch (kind) {
    // binary
    case ExprNode::add:
    case ExprNode::addEquals:
        return ADD;
    case ExprNode::subtract:
    case ExprNode::subtractEquals:
        return SUBTRACT;
    case ExprNode::multiply:
    case ExprNode::multiplyEquals:
        return MULTIPLY;
    case ExprNode::divide:
    case ExprNode::divideEquals:
        return DIVIDE;
    case ExprNode::modulo:
    case ExprNode::moduloEquals:
        return REMAINDER;
    case ExprNode::leftShift:
    case ExprNode::leftShiftEquals:
        return SHIFTLEFT;
    case ExprNode::rightShift:
    case ExprNode::rightShiftEquals:
        return SHIFTRIGHT;
    case ExprNode::logicalRightShift:
    case ExprNode::logicalRightShiftEquals:
        return USHIFTRIGHT;
    case ExprNode::bitwiseAnd:
    case ExprNode::bitwiseAndEquals:
        return AND;
    case ExprNode::bitwiseXor:
    case ExprNode::bitwiseXorEquals:
        return XOR;
    case ExprNode::bitwiseOr:
    case ExprNode::bitwiseOrEquals:
        return OR;
    // unary
    case ExprNode::plus:
        return POSATE;
    case ExprNode::minus:
        return NEGATE;
    case ExprNode::complement:
        return BITNOT;

   // relational
    case ExprNode::In:
        return COMPARE_IN;
    case ExprNode::Instanceof:
        return INSTANCEOF;

    case ExprNode::equal:
        return COMPARE_EQ;
    case ExprNode::lessThan:
        return COMPARE_LT;
    case ExprNode::lessThanOrEqual:
        return COMPARE_LE;
    case ExprNode::identical:
        return STRICT_EQ;

  // these get reversed by the generator
    case ExprNode::notEqual:        
        return COMPARE_EQ;
    case ExprNode::greaterThan:
        return COMPARE_LT;
    case ExprNode::greaterThanOrEqual:
        return COMPARE_LE;
    case ExprNode::notIdentical:
        return STRICT_EQ;


    default:
        NOT_REACHED("Unimplemented kind");
        return NOP;
    }
}


static bool generatedBoolean(ExprNode *p)
{
    switch (p->getKind()) {
    case ExprNode::parentheses: 
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            return generatedBoolean(u->op);
        }
    case ExprNode::True:
    case ExprNode::False:
    case ExprNode::equal:
    case ExprNode::notEqual:
    case ExprNode::lessThan:
    case ExprNode::lessThanOrEqual:
    case ExprNode::greaterThan:
    case ExprNode::greaterThanOrEqual:
    case ExprNode::identical:
    case ExprNode::notIdentical:
    case ExprNode::In:
    case ExprNode::Instanceof:
    case ExprNode::logicalAnd:
    case ExprNode::logicalXor:
    case ExprNode::logicalOr:
        return true;
    default:
        NOT_REACHED("I shouldn't be here."); /* quiet linux warnings */
    }
    return false;
}



/*
    if trueBranch OR falseBranch are not null, the sub-expression should generate
    a conditional branch to the appropriate target. If either branch is NULL, it
    indicates that the label is immediately forthcoming.
*/
Result ICodeGenerator::genExpr(ExprNode *p, 
                                    bool needBoolValueInBranch, 
                                    Label *trueBranch, 
                                    Label *falseBranch)
{
    Register ret = NotARegister;
    switch (p->getKind()) {
    case ExprNode::True:
        if (trueBranch || falseBranch) {
            if (needBoolValueInBranch)
                ret = loadValue(kTrue);
            if (trueBranch)
                branch(trueBranch);
        }
        else
            ret = loadValue(kTrue);
        break;
    case ExprNode::False:
        if (trueBranch || falseBranch) {
            if (needBoolValueInBranch)
                ret = loadValue(kFalse);
            if (falseBranch)
                branch(falseBranch);
        }
        else
            ret = loadValue(kFalse);
        break;
    case ExprNode::parentheses:
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            ret = genExpr(u->op, needBoolValueInBranch, trueBranch, falseBranch).reg;
        }
        break;
    case ExprNode::New:
        {
            // FIXME - handle args, etc...
            ret = newObject();
        }
        break;
    case ExprNode::call : 
        {
            InvokeExprNode *i = static_cast<InvokeExprNode *>(p);
            Register fn = genExpr(i->op).reg;
            RegisterList args;
            ExprPairList *p = i->pairs;
            while (p) {
                args.push_back(genExpr(p->value).reg);
                p = p->next;
            }
            ret = call(fn, args);
        }
        break;
    case ExprNode::index :
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register base = genExpr(b->op1).reg;       
            Register index = genExpr(b->op2).reg;
            ret = getElement(base, index);
        }
        break;
    case ExprNode::dot :
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register base = genExpr(b->op1).reg;            
            ret = getProperty(base, static_cast<IdentifierExprNode *>(b->op2)->name);
        }
        break;
    case ExprNode::identifier :
        {
            if (!mWithinWith) {
                Register v = findVariable((static_cast<IdentifierExprNode *>(p))->name);
                if (v != NotARegister)
                    ret = v;
                else
                    ret = loadName((static_cast<IdentifierExprNode *>(p))->name);
            }
            else
                ret = loadName((static_cast<IdentifierExprNode *>(p))->name);
        }
        break;
    case ExprNode::number :
        ret = loadImmediate((static_cast<NumberExprNode *>(p))->value);
        break;
    case ExprNode::string :
        ret = loadString(mWorld->identifiers[(static_cast<StringExprNode *>(p))->str]);
        break;
    case ExprNode::preIncrement: 
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            if (u->op->getKind() == ExprNode::dot) {
                BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                Register base = genExpr(b->op1).reg;
                ret = getProperty(base, static_cast<IdentifierExprNode *>(b->op2)->name);
                ret = op(ADD, ret, loadImmediate(1.0));
                setProperty(base, static_cast<IdentifierExprNode *>(b->op2)->name, ret);
            }
            else
                if (u->op->getKind() == ExprNode::identifier) {
                    if (!mWithinWith) {
                        Register v = findVariable((static_cast<IdentifierExprNode *>(u->op))->name);
                        if (v != NotARegister)
                            ret = op(ADD, ret, loadImmediate(1.0));
                        else {
                            ret = loadName((static_cast<IdentifierExprNode *>(u->op))->name);
                            ret = op(ADD, ret, loadImmediate(1.0));
                            saveName((static_cast<IdentifierExprNode *>(u->op))->name, ret);
                        }
                    }
                    else {
                        ret = loadName((static_cast<IdentifierExprNode *>(u->op))->name);
                        ret = op(ADD, ret, loadImmediate(1.0));
                        saveName((static_cast<IdentifierExprNode *>(u->op))->name, ret);
                    }
                }
                else
                    if (u->op->getKind() == ExprNode::index) {
                        BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                        Register base = genExpr(b->op1).reg;
                        Register index = genExpr(b->op2).reg;
                        ret = getElement(base, index);
                        ret = op(ADD, ret, loadImmediate(1.0));
                        setElement(base, index, ret);
                    }
        }
        break;
    case ExprNode::postIncrement: 
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            if (u->op->getKind() == ExprNode::dot) {
                BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                Register base = genExpr(b->op1).reg;
                ret = propertyInc(base, static_cast<IdentifierExprNode *>(b->op2)->name);
            }
            else
                if (u->op->getKind() == ExprNode::identifier) {
                    if (!mWithinWith) {
                        Register v = findVariable((static_cast<IdentifierExprNode *>(u->op))->name);
                        if (v != NotARegister)
                            ret = varInc(v);
                        else
                            ret = nameInc((static_cast<IdentifierExprNode *>(u->op))->name);
                    }
                    else
                        ret = nameInc((static_cast<IdentifierExprNode *>(u->op))->name);
                }
                else
                    if (u->op->getKind() == ExprNode::index) {
                        BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                        Register base = genExpr(b->op1).reg;
                        Register index = genExpr(b->op2).reg;
                        ret = elementInc(base, index);
                    }
        }
        break;
    case ExprNode::preDecrement: 
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            if (u->op->getKind() == ExprNode::dot) {
                BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                Register base = genExpr(b->op1).reg;
                ret = getProperty(base, static_cast<IdentifierExprNode *>(b->op2)->name);
                ret = op(SUBTRACT, ret, loadImmediate(1.0));
                setProperty(base, static_cast<IdentifierExprNode *>(b->op2)->name, ret);
            }
            else
                if (u->op->getKind() == ExprNode::identifier) {
                    if (!mWithinWith) {
                        Register v = findVariable((static_cast<IdentifierExprNode *>(u->op))->name);
                        if (v != NotARegister)
                            ret = op(SUBTRACT, ret, loadImmediate(1.0));
                        else {
                            ret = loadName((static_cast<IdentifierExprNode *>(u->op))->name);
                            ret = op(SUBTRACT, ret, loadImmediate(1.0));
                            saveName((static_cast<IdentifierExprNode *>(u->op))->name, ret);
                        }
                    }
                    else {
                        ret = loadName((static_cast<IdentifierExprNode *>(u->op))->name);
                        ret = op(SUBTRACT, ret, loadImmediate(1.0));
                        saveName((static_cast<IdentifierExprNode *>(u->op))->name, ret);
                    }
                }
                else
                    if (u->op->getKind() == ExprNode::index) {
                        BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                        Register base = genExpr(b->op1).reg;
                        Register index = genExpr(b->op2).reg;
                        ret = getElement(base, index);
                        ret = op(SUBTRACT, ret, loadImmediate(1.0));
                        setElement(base, index, ret);
                    }
        }
        break;
    case ExprNode::postDecrement: 
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            if (u->op->getKind() == ExprNode::dot) {
                BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                Register base = genExpr(b->op1).reg;
                ret = propertyDec(base, static_cast<IdentifierExprNode *>(b->op2)->name);
            }
            else
                if (u->op->getKind() == ExprNode::identifier) {
                    if (!mWithinWith) {
                        Register v = findVariable((static_cast<IdentifierExprNode *>(u->op))->name);
                        if (v != NotARegister)
                            ret = varDec(v);
                        else
                            ret = nameDec((static_cast<IdentifierExprNode *>(u->op))->name);
                    }
                    else
                        ret = nameDec((static_cast<IdentifierExprNode *>(u->op))->name);
                }
                else
                    if (u->op->getKind() == ExprNode::index) {
                        BinaryExprNode *b = static_cast<BinaryExprNode *>(u->op);
                        Register base = genExpr(b->op1).reg;
                        Register index = genExpr(b->op2).reg;
                        ret = elementInc(base, index);
                    }
        }
        break;
    case ExprNode::plus:
    case ExprNode::minus:
    case ExprNode::complement:
        {
            UnaryExprNode *u = static_cast<UnaryExprNode *>(p);
            Register r = genExpr(u->op).reg;
            ret = op(mapExprNodeToICodeOp(p->getKind()), r);
        }
        break;
    case ExprNode::add:
    case ExprNode::subtract:
    case ExprNode::multiply:
    case ExprNode::divide:
    case ExprNode::modulo:
    case ExprNode::leftShift:
    case ExprNode::rightShift:
    case ExprNode::logicalRightShift:
    case ExprNode::bitwiseAnd:
    case ExprNode::bitwiseXor:
    case ExprNode::bitwiseOr:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register r1 = genExpr(b->op1).reg;
            Register r2 = genExpr(b->op2).reg;
            ret = op(mapExprNodeToICodeOp(p->getKind()), r1, r2);
        }
        break;
    case ExprNode::assignment:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            ret = genExpr(b->op2).reg;
            if (b->op1->getKind() == ExprNode::identifier) {
                if (!mWithinWith) {
                    Register v = findVariable((static_cast<IdentifierExprNode *>(b->op1))->name);
                    if (v != NotARegister)
                        move(v, ret);
                    else
                        saveName((static_cast<IdentifierExprNode *>(b->op1))->name, ret);
                }
                else
                    saveName((static_cast<IdentifierExprNode *>(b->op1))->name, ret);
            }
            else
                if (b->op1->getKind() == ExprNode::dot) {
                    BinaryExprNode *lb = static_cast<BinaryExprNode *>(b->op1);
                    Register base = genExpr(lb->op1).reg;            
                    setProperty(base, static_cast<IdentifierExprNode *>(lb->op2)->name, ret);
                }
                else
                    if (b->op1->getKind() == ExprNode::index) {
                        BinaryExprNode *lb = static_cast<BinaryExprNode *>(b->op1);
                        Register base = genExpr(lb->op1).reg;
                        Register index = genExpr(lb->op2).reg;
                        setElement(base, index, ret);
                    }
        }
        break;
    case ExprNode::addEquals:
    case ExprNode::subtractEquals:
    case ExprNode::multiplyEquals:
    case ExprNode::divideEquals:
    case ExprNode::moduloEquals:
    case ExprNode::leftShiftEquals:
    case ExprNode::rightShiftEquals:
    case ExprNode::logicalRightShiftEquals:
    case ExprNode::bitwiseAndEquals:
    case ExprNode::bitwiseXorEquals:
    case ExprNode::bitwiseOrEquals:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            ret = genExpr(b->op2).reg;
            if (b->op1->getKind() == ExprNode::identifier) {
                if (!mWithinWith) {
                    Register v = findVariable((static_cast<IdentifierExprNode *>(b->op1))->name);
                    if (v != NotARegister) {
                        ret = op(mapExprNodeToICodeOp(p->getKind()), v, ret);
                        move(v, ret);
                    }
                    else {
                        v = loadName((static_cast<IdentifierExprNode *>(b->op1))->name);
                        ret = op(mapExprNodeToICodeOp(p->getKind()), v, ret);
                        saveName((static_cast<IdentifierExprNode *>(b->op1))->name, ret);
                    }
                }
                else {
                    Register v = loadName((static_cast<IdentifierExprNode *>(b->op1))->name);
                    ret = op(mapExprNodeToICodeOp(p->getKind()), v, ret);
                    saveName((static_cast<IdentifierExprNode *>(b->op1))->name, ret);
                }
            }
            else
                if (b->op1->getKind() == ExprNode::dot) {
                    BinaryExprNode *lb = static_cast<BinaryExprNode *>(b->op1);
                    Register base = genExpr(lb->op1).reg;
                    Register v = getProperty(base, static_cast<IdentifierExprNode *>(lb->op2)->name);
                    ret = op(mapExprNodeToICodeOp(p->getKind()), v, ret);
                    setProperty(base, static_cast<IdentifierExprNode *>(lb->op2)->name, ret);
                }
                else
                    if (b->op1->getKind() == ExprNode::index) {
                        BinaryExprNode *lb = static_cast<BinaryExprNode *>(b->op1);
                        Register base = genExpr(lb->op1).reg;
                        Register index = genExpr(lb->op2).reg;
                        Register v = getElement(base, index);
                        ret = op(mapExprNodeToICodeOp(p->getKind()), v, ret);
                        setElement(base, index, ret);
                    }
        }
        break;
    case ExprNode::equal:
    case ExprNode::lessThan:
    case ExprNode::lessThanOrEqual:
    case ExprNode::identical:
    case ExprNode::In:
    case ExprNode::Instanceof:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register r1 = genExpr(b->op1).reg;
            Register r2 = genExpr(b->op2).reg;
            ret = op(mapExprNodeToICodeOp(p->getKind()), r1, r2);
            if (trueBranch || falseBranch) {
                if (trueBranch == NULL)
                    branchFalse(falseBranch, ret);
                else {
                    branchTrue(trueBranch, ret);
                    if (falseBranch)
                        branch(falseBranch);
                }
            }
        }
        break;
    case ExprNode::greaterThan:
    case ExprNode::greaterThanOrEqual:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register r1 = genExpr(b->op1).reg;
            Register r2 = genExpr(b->op2).reg;
            ret = op(mapExprNodeToICodeOp(p->getKind()), r2, r1);   // will return reverse case
            if (trueBranch || falseBranch) {
                if (trueBranch == NULL)
                    branchFalse(falseBranch, ret);
                else {
                    branchTrue(trueBranch, ret);
                    if (falseBranch)
                        branch(falseBranch);
                }
            }
        }
        break;

    case ExprNode::notEqual:
    case ExprNode::notIdentical:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            Register r1 = genExpr(b->op1).reg;
            Register r2 = genExpr(b->op2).reg;
            ret = op(mapExprNodeToICodeOp(p->getKind()), r1, r2);
            if (trueBranch || falseBranch) {
                if (trueBranch == NULL)
                    branchFalse(falseBranch, ret);
                else {
                    branchTrue(trueBranch, ret);
                    if (falseBranch)
                        branch(falseBranch);
                }
            }
            else
                ret = logicalNot(ret);

        }
        break;
    
    case ExprNode::logicalAnd:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            if (trueBranch || falseBranch) {
                genExpr(b->op1, needBoolValueInBranch, NULL, falseBranch);
                genExpr(b->op2, needBoolValueInBranch, trueBranch, falseBranch);
            }
            else {
                Label *fBranch = getLabel();
                Register r1 = genExpr(b->op1, true, NULL, fBranch).reg;
                if (!generatedBoolean(b->op1)) {
                    r1 = test(r1);
                    branchFalse(fBranch, r1);
                }
                Register r2 = genExpr(b->op2).reg;
                if (!generatedBoolean(b->op2)) {
                    r2 = test(r2);
                }
                if (r1 != r2)       // FIXME, need a way to specify a dest???
                    move(r1, r2);
                setLabel(fBranch);
                ret = r1;
            }
        }
        break;
    case ExprNode::logicalOr:
        {
            BinaryExprNode *b = static_cast<BinaryExprNode *>(p);
            if (trueBranch || falseBranch) {
                genExpr(b->op1, needBoolValueInBranch, trueBranch, NULL);
                genExpr(b->op2, needBoolValueInBranch, trueBranch, falseBranch);
            }
            else {
                Label *tBranch = getLabel();
                Register r1 = genExpr(b->op1, true, tBranch, NULL).reg;
                if (!generatedBoolean(b->op1)) {
                    r1 = test(r1);
                    branchTrue(tBranch, r1);
                }
                Register r2 = genExpr(b->op2).reg;
                if (!generatedBoolean(b->op2)) {
                    r2 = test(r2);
                }
                if (r1 != r2)       // FIXME, need a way to specify a dest???
                    move(r1, r2);
                setLabel(tBranch);
                ret = r1;
            }
        }
        break;

    case ExprNode::conditional:
        {
            TernaryExprNode *t = static_cast<TernaryExprNode *>(p);
            Label *fBranch = getLabel();
            Label *beyondBranch = getLabel();
            Register c = genExpr(t->op1, false, NULL, fBranch).reg;
            if (!generatedBoolean(t->op1))
                branchFalse(fBranch, test(c));
            Register r1 = genExpr(t->op2).reg;
            branch(beyondBranch);
            setLabel(fBranch);
            Register r2 = genExpr(t->op3).reg;
            if (r1 != r2)       // FIXME, need a way to specify a dest???
                move(r1, r2);
            setLabel(beyondBranch);
            ret = r1;
        }
        break;

    case ExprNode::objectLiteral:
        {
            ret = newObject();

            PairListExprNode *plen = static_cast<PairListExprNode *>(p);
            ExprPairList *e = plen->pairs;
            while (e) {
                if (e->field && e->value && (e->field->getKind() == ExprNode::identifier))
                    setProperty(ret, (static_cast<IdentifierExprNode *>(e->field))->name, genExpr(e->value).reg);
                e = e->next;
            }
        }
        break;

    default:
        {
            NOT_REACHED("Unsupported ExprNode kind");
        }   
    }
    return Result(ret, Any_Type);
}

/*
 need pre-pass to find: 
    variable & function definitions,
    contains 'with' or 'eval'
    contains 'try {} catch {} finally {}'
*/


bool LabelEntry::containsLabel(const StringAtom *label)
{
    if (labelSet) {
        for (LabelSet::iterator i = labelSet->begin(); i != labelSet->end(); i++)
            if ( (*i) == label )
                return true;
    }
    return false;
}

void ICodeGenerator::preprocess(StmtNode *p)
{
    switch (p->getKind()) {
    case StmtNode::block:
        {
            BlockStmtNode *b = static_cast<BlockStmtNode *>(p);
            StmtNode *s = b->statements;
            while (s) {
                preprocess(s);
                s = s->next;
            }            
        }
        break;
    case StmtNode::Var:
        {
            VariableStmtNode *vs = static_cast<VariableStmtNode *>(p);
            VariableBinding *v = vs->bindings;
            while (v)  {
                if (v->name && (v->name->getKind() == ExprNode::identifier))
                    if (v->type->getKind() == ExprNode::identifier)
                        allocateVariable((static_cast<IdentifierExprNode *>(v->name))->name,
                                            (static_cast<IdentifierExprNode *>(v->type))->name);
                    else 
                        allocateVariable((static_cast<IdentifierExprNode *>(v->name))->name);
                v = v->next;
            }
        }
        break;
    case StmtNode::DoWhile:
        {
            UnaryStmtNode *d = static_cast<UnaryStmtNode *>(p);
            preprocess(d->stmt);
        }
        break;
    case StmtNode::While:
        {
            UnaryStmtNode *w = static_cast<UnaryStmtNode *>(p);
            preprocess(w->stmt);
        }
        break;
    case StmtNode::For:
        {
            ForStmtNode *f = static_cast<ForStmtNode *>(p);
            if (f->initializer) 
                preprocess(f->initializer);
            preprocess(f->stmt);
        }
        break;
    case StmtNode::If:
        {
            UnaryStmtNode *i = static_cast<UnaryStmtNode *>(p);
            preprocess(i->stmt);
        }
        break;
    case StmtNode::IfElse:
        {
            BinaryStmtNode *i = static_cast<BinaryStmtNode *>(p);
            preprocess(i->stmt);
            preprocess(i->stmt2);
        }
        break;
    case StmtNode::With:
        {
            UnaryStmtNode *w = static_cast<UnaryStmtNode *>(p);
            preprocess(w->stmt);
        }
        break;
    case StmtNode::Switch:
        {
            SwitchStmtNode *sw = static_cast<SwitchStmtNode *>(p);
            StmtNode *s = sw->statements;
            while (s) {
                if (s->getKind() != StmtNode::Case)
                    preprocess(s);
                s = s->next;
            }
        }
        break;
    case StmtNode::label:
        {
            LabelStmtNode *l = static_cast<LabelStmtNode *>(p);
            preprocess(l->stmt);
        }
        break;
    case StmtNode::Try:
        {
            TryStmtNode *t = static_cast<TryStmtNode *>(p);
            genStmt(t->stmt);
            if (t->catches) {
                CatchClause *c = t->catches;
                while (c) {
                    preprocess(c->stmt);
                    c = c->next;
                }
            }
            if (t->finally)
                genStmt(t->finally);
        }
        break;
   }
}

Register ICodeGenerator::genStmt(StmtNode *p, LabelSet *currentLabelSet)
{
    Register ret = NotARegister;

    startStatement(p->pos);
    if (exceptionRegister == NotARegister) {
        exceptionRegister = grabRegister(mWorld->identifiers[widenCString("__exceptionObject__")]);
    }

    switch (p->getKind()) {
    case StmtNode::Function:
        {
            FunctionStmtNode *f = static_cast<FunctionStmtNode *>(p);
            ICodeGenerator icg(mWorld);
            VariableBinding *v = f->function.parameters;
            while (v) {
                if (v->name && (v->name->getKind() == ExprNode::identifier))
                    icg.allocateParameter((static_cast<IdentifierExprNode *>(v->name))->name);
                v = v->next;
            }
            icg.preprocess(f->function.body);
            icg.genStmt(f->function.body);
            //stdOut << icg;
            ICodeModule *icm = icg.complete();
            if (f->function.name->getKind() == ExprNode::identifier)
                defFunction((static_cast<IdentifierExprNode *>(f->function.name))->name, icm);
        }
        break;
    case StmtNode::Var:
        {
            VariableStmtNode *vs = static_cast<VariableStmtNode *>(p);
            VariableBinding *v = vs->bindings;
            while (v)  {
                if (v->name && v->initializer) {
                    if (v->name->getKind() == ExprNode::identifier) {
                        if (!mWithinWith) {
                            Register r = genExpr(v->name).reg;
                            Register val = genExpr(v->initializer).reg;
                            move(r, val);
                        }
                        else {
                            Register val = genExpr(v->initializer).reg;
                            saveName((static_cast<IdentifierExprNode *>(v->name))->name, val);
                        }
                    }
                    // XXX what's the else case here, when does a VariableBinding NOT have an identifier child?
                }
                v = v->next;
            }
        }
        break;
    case StmtNode::expression:
        {
            ExprStmtNode *e = static_cast<ExprStmtNode *>(p);
            ret = genExpr(e->expr).reg;
        }
        break;
    case StmtNode::Throw:
        {
            ExprStmtNode *e = static_cast<ExprStmtNode *>(p);
            throwStmt(genExpr(e->expr).reg);
        }
        break;
    case StmtNode::Return:
        {
            ExprStmtNode *e = static_cast<ExprStmtNode *>(p);
            if (e->expr)
                returnStmt(ret = genExpr(e->expr).reg);
            else
                returnStmt(NotARegister);
        }
        break;
    case StmtNode::If:
        {
            Label *falseLabel = getLabel();
            UnaryStmtNode *i = static_cast<UnaryStmtNode *>(p);
            Register c = genExpr(i->expr, false, NULL, falseLabel).reg;
            if (!generatedBoolean(i->expr))
                branchFalse(falseLabel, test(c));
            genStmt(i->stmt);
            setLabel(falseLabel);
        }
        break;
    case StmtNode::IfElse:
        {
            Label *falseLabel = getLabel();
            Label *beyondLabel = getLabel();
            BinaryStmtNode *i = static_cast<BinaryStmtNode *>(p);
            Register c = genExpr(i->expr, false, NULL, falseLabel).reg;
            if (!generatedBoolean(i->expr))
                branchFalse(falseLabel, test(c));
            genStmt(i->stmt);
            branch(beyondLabel);
            setLabel(falseLabel);
            genStmt(i->stmt2);
            setLabel(beyondLabel);
        }
        break;
    case StmtNode::With:
        {
            UnaryStmtNode *w = static_cast<UnaryStmtNode *>(p);
            Register o = genExpr(w->expr).reg;
            bool withinWith = mWithinWith;
            mWithinWith = true;
            beginWith(o);
            genStmt(w->stmt);
            endWith();
            mWithinWith = withinWith;
        }
        break;
    case StmtNode::Switch:
        {
            Label *defaultLabel = NULL;
            LabelEntry *e = new LabelEntry(currentLabelSet, getLabel());
            mLabelStack.push_back(e);
            SwitchStmtNode *sw = static_cast<SwitchStmtNode *>(p);
            Register sc = genExpr(sw->expr).reg;
            StmtNode *s = sw->statements;
            // ECMA requires case & default statements to be immediate children of switch
            // unlike C where they can be arbitrarily deeply nested in other statements.    
            Label *nextCaseLabel = NULL;
            GenericBranch *lastBranch = NULL;
            while (s) {
                if (s->getKind() == StmtNode::Case) {
                    ExprStmtNode *c = static_cast<ExprStmtNode *>(s);
                    if (c->expr) {
                        if (nextCaseLabel)
                            setLabel(nextCaseLabel);
                        nextCaseLabel = getLabel();
                        Register r = genExpr(c->expr).reg;
                        Register eq = op(COMPARE_EQ, r, sc);
                        lastBranch = branchFalse(nextCaseLabel, eq);
                    }
                    else {
                        defaultLabel = getLabel();
                        setLabel(defaultLabel);
                    }
                }
                else
                    genStmt(s);
                s = s->next;
            }
            if (nextCaseLabel)
                setLabel(nextCaseLabel);
            if (defaultLabel && lastBranch)
                lastBranch->setTarget(defaultLabel);

            setLabel(e->breakLabel);
            mLabelStack.pop_back();
        }
        break;
    case StmtNode::DoWhile:
        {
            LabelEntry *e = new LabelEntry(currentLabelSet, getLabel(), getLabel());
            mLabelStack.push_back(e);
            UnaryStmtNode *d = static_cast<UnaryStmtNode *>(p);
            Label *doBodyTopLabel = getLabel();
            setLabel(doBodyTopLabel);
            genStmt(d->stmt);
            setLabel(e->continueLabel);
            Register c = genExpr(d->expr, false, doBodyTopLabel, NULL).reg;
            if (!generatedBoolean(d->expr))
                branchTrue(doBodyTopLabel, test(c));
            setLabel(e->breakLabel);
            mLabelStack.pop_back();
        }
        break;
    case StmtNode::While:
        {
            LabelEntry *e = new LabelEntry(currentLabelSet, getLabel(), getLabel());
            mLabelStack.push_back(e);
            branch(e->continueLabel);
            
            UnaryStmtNode *w = static_cast<UnaryStmtNode *>(p);

            Label *whileBodyTopLabel = getLabel();
            setLabel(whileBodyTopLabel);
            genStmt(w->stmt);

            setLabel(e->continueLabel);
            Register c = genExpr(w->expr, false, whileBodyTopLabel, NULL).reg;
            if (!generatedBoolean(w->expr))
                branchTrue(whileBodyTopLabel, test(c));

            setLabel(e->breakLabel);
            mLabelStack.pop_back();
        }
        break;
    case StmtNode::For:
        {
            LabelEntry *e = new LabelEntry(currentLabelSet, getLabel(), getLabel());
            mLabelStack.push_back(e);

            ForStmtNode *f = static_cast<ForStmtNode *>(p);
            if (f->initializer) 
                genStmt(f->initializer);
            Label *forTestLabel = getLabel();
            branch(forTestLabel);

            Label *forBlockTop = getLabel();
            setLabel(forBlockTop);
            genStmt(f->stmt);
            
            setLabel(e->continueLabel);
            if (f->expr3)
                genExpr(f->expr3).reg;
            
            setLabel(forTestLabel);
            if (f->expr2) {
                Register c = genExpr(f->expr2, false, forBlockTop, NULL).reg;
                if (!generatedBoolean(f->expr2))
                    branchTrue(forBlockTop, test(c));
            }
            
            setLabel(e->breakLabel);

            mLabelStack.pop_back();
        }
        break;
    case StmtNode::block:
        {
            BlockStmtNode *b = static_cast<BlockStmtNode *>(p);
            StmtNode *s = b->statements;
            while (s) {
                genStmt(s);
                s = s->next;
            }            
        }
        break;

    case StmtNode::label:
        {
            LabelStmtNode *l = static_cast<LabelStmtNode *>(p);
            // ok, there's got to be a cleverer way of doing this...
            if (currentLabelSet != NULL) {
                currentLabelSet = new LabelSet();
                currentLabelSet->push_back(&l->name);
                genStmt(l->stmt, currentLabelSet);
                delete currentLabelSet;
            }
            else {
                currentLabelSet->push_back(&l->name);
                genStmt(l->stmt, currentLabelSet);
                currentLabelSet->pop_back();
            }
        }
        break;

    case StmtNode::Break:
        {
            GoStmtNode *g = static_cast<GoStmtNode *>(p);
            if (g->label) {
                LabelEntry *e = NULL;
                for (LabelStack::reverse_iterator i = mLabelStack.rbegin(); i != mLabelStack.rend(); i++) {
                    e = (*i);
                    if (e->containsLabel(g->name))
                        break;
                }
                if (e) {
                    ASSERT(e->breakLabel);
                    branch(e->breakLabel);
                }
                else
                    NOT_REACHED("break label not in label set");
            }
            else {
                ASSERT(!mLabelStack.empty());
                LabelEntry *e = mLabelStack.back();
                ASSERT(e->breakLabel);
                branch(e->breakLabel);
            }
        }
        break;
    case StmtNode::Continue:
        {
            GoStmtNode *g = static_cast<GoStmtNode *>(p);
            if (g->label) {
                LabelEntry *e = NULL;
                for (LabelStack::reverse_iterator i = mLabelStack.rbegin(); i != mLabelStack.rend(); i++) {
                    e = (*i);
                    if (e->containsLabel(g->name))
                        break;
                }
                if (e) {
                    ASSERT(e->continueLabel);
                    branch(e->continueLabel);
                }
                else
                    NOT_REACHED("continue label not in label set");
            }
            else {
                ASSERT(!mLabelStack.empty());
                LabelEntry *e = mLabelStack.back();
                ASSERT(e->continueLabel);
                branch(e->continueLabel);
            }
        }
        break;

    case StmtNode::Try:
        {
            /*
                The finallyInvoker is a little stub used by the interpreter to
                invoke the finally handler on the (exceptional) way out of the
                try block assuming there are no catch clauses.
            */
            Register ex = NotARegister;
            TryStmtNode *t = static_cast<TryStmtNode *>(p);
            Label *catchLabel = (t->catches) ? getLabel() : NULL;
            Label *finallyInvoker = (t->finally) ? getLabel() : NULL;
            Label *finallyLabel = (t->finally) ? getLabel() : NULL;
            Label *beyondLabel = getLabel();
            beginTry(catchLabel, finallyLabel);
            genStmt(t->stmt);
            endTry();
            if (finallyLabel)
                jsr(finallyLabel);
            branch(beyondLabel);
            if (catchLabel) {
                setLabel(catchLabel);
                CatchClause *c = t->catches;
                while (c) {
                    // Bind the incoming exception ...
                    setRegisterForVariable(c->name, exceptionRegister);

                    genStmt(c->stmt);
                    if (finallyLabel)
                        jsr(finallyLabel);
                    throwStmt(exceptionRegister);
                    c = c->next;
                }
            }
            if (finallyLabel) {
                setLabel(finallyInvoker);
                jsr(finallyLabel);
                throwStmt(exceptionRegister);

                setLabel(finallyLabel);
                genStmt(t->finally);
                rts();
            }
            setLabel(beyondLabel);
        }
        break;

    default:
        NOT_REACHED("unimplemented statement kind");
    }
    resetStatement();
    return ret;
}


/************************************************************************/


Formatter& ICodeGenerator::print(Formatter& f)
{
    f << "ICG! " << (uint32)iCode->size() << "\n";
    VM::operator<<(f, *iCode);
    f << "  Src  :  Instr" << "\n";
    for (InstructionMap::iterator i = mInstructionMap->begin(); i != mInstructionMap->end(); i++)
    {
        printDec( f, (*i)->first, 6);
        f << " : ";
        printDec( f, (*i)->second, 6);
        f << "\n";
//        f << (*i)->first << " : " << (*i)->second << "\n";
    }
    return f;
}

Formatter& ICodeModule::print(Formatter& f)
{
    f << "ICM! " << (uint32)its_iCode->size() << "\n";
    return VM::operator<<(f, *its_iCode);
}

    
} // namespace ICG

} // namespace JavaScript
