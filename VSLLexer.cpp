#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "KaleidoscopeJIT.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::orc;


static std::map<char, int> BinopPrecedence;
enum token {
	//结束符
	tok_eof = -1,

	//函数
	tok_func=-2,
	tok_return=-3,

	//变量名
	tok_identifier = -4,

	//数值
	tok_number = -5,

	//if语句
	tok_if = -6,
	tok_then = -7,
	tok_else = -8,
	tok_fi=-9,

	//for语句
	tok_for = -10,
	tok_in = -11,

	//do while
	tok_do=-12,
	tok_while=-13,
	tok_done=-14,

	tok_continue=-15,

	//输出
	tok_print=-16,

};

//表达式抽象类
class ExprAST {
public:
	virtual ~ExprAST() {}
};

//数字表达式
class NumberExprAST : public ExprAST {
	double val;

public:
	NumberExprAST(double val) : val(val) {}
};

//变量表达式
class VariableExprAST : public ExprAST {
	std::string Name;

public:
	VariableExprAST(const std::string &Name) : Name(Name) {}
};

//二元表达式
class BinaryExprAST : public ExprAST {
	char Op;//操作符
	std::unique_ptr<ExprAST> LHS, RHS;//左右操作数

public:
	BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
		std::unique_ptr<ExprAST> RHS)
		: Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

//函数调用表达式
class CallExprAST : public ExprAST {
	std::string callee;//被调用函数名
	std::vector<std::unique_ptr<ExprAST>> args;//函数参数

public:
	CallExprAST(const std::string &callee,
		std::vector<std::unique_ptr<ExprAST>> args)
		: callee(callee), args(std::move(args)) {}
};
class PrototypeAST {
	std::string Name;
	std::vector<std::string> Args;
	bool IsOperator;
	unsigned Precedence; // Precedence if a binary op.

public:
	PrototypeAST(const std::string &Name, std::vector<std::string> Args,
		bool IsOperator = false, unsigned Prec = 0)
		: Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
		Precedence(Prec) {}

	Function *codegen();
	const std::string &getName() const { return Name; }

	bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
	bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

	char getOperatorName() const {
		assert(isUnaryOp() || isBinaryOp());
		return Name[Name.size() - 1];
	}

	unsigned getBinaryPrecedence() const { return Precedence; }
};


class FunctionAST {
	std::unique_ptr<PrototypeAST> Proto;
	std::unique_ptr<ExprAST> Body;

public:
	FunctionAST(std::unique_ptr<PrototypeAST> Proto,
		std::unique_ptr<ExprAST> Body)
		: Proto(std::move(Proto)), Body(std::move(Body)) {}

	Function *codegen();
};


static std::string identifierStr;
static double numVal;

static LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
static std::unique_ptr<Module> TheModule;
static std::map<std::string, AllocaInst *> NamedValues;
static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

static int CurTok;
static int getToken() {

	static int lastChar = ' ';
	while (isspace(lastChar)) {
		lastChar = getchar();
	}

	//字母开头
	if (isalpha(lastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
		identifierStr = lastChar;
		while (isalnum((lastChar = getchar())))
			identifierStr += lastChar;

		if (identifierStr == "FUNC")
			return tok_func;
		if (identifierStr == "RETURN")
			return tok_return;
		if (identifierStr == "IF")
			return tok_if;
		if (identifierStr == "ELSE")
		{
			return tok_else;
		}
		if (identifierStr == "THEN")
		{
			return tok_then;
		}
		if (identifierStr == "IF")
		{
			return tok_if;
		}
		if (identifierStr == "DO")
		{
			return tok_do;
		}
		if (identifierStr == "WHILE")
		{
			return tok_while;
		}
		if (identifierStr == "DONE")
		{
			return tok_done;
		}
		if (identifierStr == "CONTINUE")
		{
			return tok_continue;
		}
		if (identifierStr == "PRINT")
		{
			return tok_print;
		}
		return tok_identifier;
	}

	//数字开头
	if (isdigit(lastChar) || lastChar == '.')
	{
		std::string numStr;
		do
		{
			numStr += lastChar;
			lastChar = getchar();
		} while (isdigit(lastChar) || lastChar == '.');

		//TODO:这里需要在上下文进行更多的错误检查

		numVal = strtod(numStr.c_str(), 0);
		return tok_number;
	}

	//结尾符号
	if (lastChar == EOF) {
		return tok_eof;
	}

	//其他情况，直接返回该字符
	int thisChar = lastChar;
	lastChar = getchar();
	return thisChar;
}
static int getNextToken() { return CurTok = getToken(); }



static void InitializeModule() {
	// Open a new module.
	TheModule = llvm::make_unique<Module>("my cool jit", TheContext);
	TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());
}
std::unique_ptr<ExprAST> LogError(const char *Str) {
	fprintf(stderr, "Error: %s\n", Str);
	return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
	LogError(Str);
	return nullptr;
}
static std::unique_ptr<PrototypeAST> ParsePrototype() {
	std::string FnName;

	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
	unsigned BinaryPrecedence = 30;

	switch (CurTok) {
	default:
		return LogErrorP("Expected function name in prototype");
	case tok_identifier:
		FnName = identifierStr;
		Kind = 0;
		getNextToken();
		break;
	//case tok_unary:
	//	getNextToken();
	//	if (!isascii(CurTok))
	//		return LogErrorP("Expected unary operator");
	//	FnName = "unary";
	//	FnName += (char)CurTok;
	//	Kind = 1;
	//	getNextToken();
	//	break;
	//case tok_binary:
	//	getNextToken();
	//	if (!isascii(CurTok))
	//		return LogErrorP("Expected binary operator");
	//	FnName = "binary";
	//	FnName += (char)CurTok;
	//	Kind = 2;
	//	getNextToken();

	//	// Read the precedence if present.
	//	if (CurTok == tok_number) {
	//		if (numVal < 1 || numVal > 100)
	//			return LogErrorP("Invalid precedecnce: must be 1..100");
	//		BinaryPrecedence = (unsigned)numVal;
	//		getNextToken();
	//	}
	//	break;
	}

	if (CurTok != '(')
		return LogErrorP("Expected '(' in prototype");

	std::vector<std::string> ArgNames;
	while (getNextToken() == tok_identifier)
		ArgNames.push_back(identifierStr);
	if (CurTok != ')')
		return LogErrorP("Expected ')' in prototype");

	// success.
	getNextToken(); // eat ')'.

	// Verify right number of names for operator.
	if (Kind && ArgNames.size() != Kind)
		return LogErrorP("Invalid number of operands for operator");

	return llvm::make_unique<PrototypeAST>(FnName, ArgNames, Kind != 0,
		BinaryPrecedence);
}
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = llvm::make_unique<NumberExprAST>(numVal);
	getNextToken(); // consume the number
	return std::move(Result);
}
static std::unique_ptr<ExprAST> ParsePrimary();
static int GetTokPrecedence();
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
	std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();

		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec < ExprPrec)
			return LHS;

		// Okay, we know this is a binop.
		int BinOp = CurTok;
		getNextToken(); // eat binop

		// Parse the unary expression after the binary operator.
		auto RHS = ParsePrimary();
		if (!RHS)
			return nullptr;

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		int NextPrec = GetTokPrecedence();
		if (TokPrec < NextPrec) {
			RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
			if (!RHS)
				return nullptr;
		}

		// Merge LHS/RHS.
		LHS =
			llvm::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
	}
}
static std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParsePrimary();
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	if (CurTok != ')')
		return LogError("expected ')'");
	getNextToken(); // eat ).
	return V;
}


static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = identifierStr;

	getNextToken(); // eat identifier.

	if (CurTok != '(') // Simple variable ref.
		return llvm::make_unique<VariableExprAST>(IdName);

	// Call.
	getNextToken(); // eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if (CurTok != ')') {
		while (true) {
			if (auto Arg = ParseExpression())
				Args.push_back(std::move(Arg));
			else
				return nullptr;

			if (CurTok == ')')
				break;

			if (CurTok != ',')
				return LogError("Expected ')' or ',' in argument list");
			getNextToken();
		}
	}

	// Eat the ')'.
	getNextToken();

	return llvm::make_unique<CallExprAST>(IdName, std::move(Args));
}
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch (CurTok) {
	default:
		return LogError("unknown token when expecting an expression");
	case tok_identifier:
		return ParseIdentifierExpr();
	case tok_number:
		return ParseNumberExpr();
	case '(':
		return ParseParenExpr();
	//case tok_if:
	//	return ParseIfExpr();
	//case tok_for:
	//	return ParseForExpr();
	//case tok_var:
	//	return ParseVarExpr();
	}
}
//static std::unique_ptr<ExprAST> ParseUnary() {
//	// If the current token is not an operator, it must be a primary expr.
//	if (!isascii(CurTok) || CurTok == '(' || CurTok == ',')
//		return ParsePrimary();
//
//	// If this is a unary operator, read it.
//	int Opc = CurTok;
//	getNextToken();
//	if (auto Operand = ParseUnary())
//		return llvm::make_unique<UnaryExprAST>(Opc, std::move(Operand));
//	return nullptr;
//}

static int GetTokPrecedence() {
	if (!isascii(CurTok))
		return -1;

	// Make sure it's a declared binop.
	int TokPrec = BinopPrecedence[CurTok];
	if (TokPrec <= 0)
		return -1;
	return TokPrec;
}


static std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat def.
	auto Proto = ParsePrototype();
	if (!Proto)
		return nullptr;

	if (auto E = ParseExpression())
		return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	return nullptr;
}
static void HandleDefinition() {
	/*if (auto FnAST = ParseDefinition()) {
		if (auto *FnIR = FnAST->codegen()) {
			fprintf(stderr, "Read function definition:");
			FnIR->print(errs());
			fprintf(stderr, "\n");
			TheJIT->addModule(std::move(TheModule));
			InitializeModule();
		}
	}*/
	if (ParseDefinition())
	{
		fprintf(stderr, "Parsed a function definition.\n");
	}
	else {
		// Skip token for error recovery.
		getNextToken();
	}
}
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	if (auto E = ParseExpression()) {
		// Make an anonymous proto.
		auto Proto = llvm::make_unique<PrototypeAST>("__anon_expr",
			std::vector<std::string>());
		return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}
static void HandleTopLevelExpression() {
	if (ParseTopLevelExpr())
	{
		fprintf(stderr, "Parsed a top-level xepr\n");
	}
	else {
		getNextToken();
	}
}
static void MainLoop() {
	while (true) {
		fprintf(stderr, "ready> ");
		switch (CurTok) {
		case tok_eof:
			return;
		case ';': // ignore top-level semicolons.
			getNextToken();
			break;
		case tok_func:
			HandleDefinition();
			break;
		/*case tok_extern:
			HandleExtern();
			break;*/
		default:
			HandleTopLevelExpression();
			break;
		}
	}
}
int main() {
	InitializeNativeTarget();
	InitializeNativeTargetAsmPrinter();
	InitializeNativeTargetAsmParser();

	// Install standard binary operators.
	// 1 is lowest precedence.
	BinopPrecedence['='] = 2;
	BinopPrecedence['<'] = 10;
	BinopPrecedence['+'] = 20;
	BinopPrecedence['-'] = 20;
	BinopPrecedence['*'] = 40; // highest.

	// Prime the first token.
	fprintf(stderr, "ready> ");
	getNextToken();

	TheJIT = llvm::make_unique<KaleidoscopeJIT>();

	InitializeModule();

	// Run the main "interpreter loop" now.
	MainLoop();

  return 0;
}

