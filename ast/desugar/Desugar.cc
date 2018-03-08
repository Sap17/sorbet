#include <algorithm>

#include "Desugar.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/verifier/verifier.h"
#include "common/common.h"
#include "core/Names/desugar.h"
#include "core/errors/desugar.h"
#include "core/errors/internal.h"

namespace ruby_typer {
namespace ast {
namespace desugar {

using namespace std;

namespace {

unique_ptr<Expression> node2TreeImpl(core::MutableContext ctx, unique_ptr<parser::Node> what, u2 &uniqueCounter);

pair<MethodDef::ARGS_store, unique_ptr<Expression>> desugarArgsAndBody(core::MutableContext ctx, core::Loc loc,
                                                                       unique_ptr<parser::Node> &argnode,
                                                                       unique_ptr<parser::Node> &bodynode,
                                                                       u2 &uniqueCounter) {
    MethodDef::ARGS_store args;
    InsSeq::STATS_store destructures;

    if (auto *oargs = parser::cast_node<parser::Args>(argnode.get())) {
        args.reserve(oargs->args.size());
        for (auto &arg : oargs->args) {
            if (parser::Mlhs *lhs = parser::cast_node<parser::Mlhs>(arg.get())) {
                core::NameRef temporary = ctx.state.freshNameUnique(core::UniqueNameKind::Desugar,
                                                                    core::Names::destructureArg(), ++uniqueCounter);
                args.emplace_back(MK::Local(arg->loc, temporary));
                unique_ptr<parser::Node> lvarNode = make_unique<parser::LVar>(arg->loc, temporary);
                unique_ptr<parser::Node> destructure = make_unique<parser::Masgn>(arg->loc, move(arg), move(lvarNode));
                destructures.emplace_back(node2TreeImpl(ctx, move(destructure), uniqueCounter));
            } else {
                args.emplace_back(node2TreeImpl(ctx, move(arg), uniqueCounter));
            }
        }
    } else if (argnode.get() == nullptr) {
        // do nothing
    } else {
        auto node = argnode.get();
        Error::raise("not implemented: ", demangle(typeid(*node).name()));
    }

    auto body = node2TreeImpl(ctx, move(bodynode), uniqueCounter);
    if (!destructures.empty()) {
        core::Loc bodyLoc = body->loc;
        if (bodyLoc.is_none()) {
            bodyLoc = loc;
        }
        body = MK::InsSeq(loc, move(destructures), move(body));
    }

    return make_pair(move(args), move(body));
}

unique_ptr<Expression> desugarDString(core::MutableContext ctx, core::Loc loc, parser::NodeVec nodes,
                                      u2 &uniqueCounter) {
    if (nodes.empty()) {
        return make_unique<StringLit>(loc, core::Names::empty());
    }
    auto it = nodes.begin();
    auto end = nodes.end();
    unique_ptr<Expression> res;
    unique_ptr<Expression> first = node2TreeImpl(ctx, move(*it), uniqueCounter);
    if (isa_tree<StringLit>(first.get())) {
        res = move(first);
    } else {
        auto pieceLoc = first->loc;
        res = MK::Send0(pieceLoc, move(first), core::Names::to_s());
    }
    ++it;
    for (; it != end; ++it) {
        auto &stat = *it;
        unique_ptr<Expression> narg = node2TreeImpl(ctx, move(stat), uniqueCounter);
        if (!isa_tree<StringLit>(narg.get())) {
            auto pieceLoc = narg->loc;
            narg = MK::Send0(pieceLoc, move(narg), core::Names::to_s());
        }
        auto n = MK::Send1(loc, move(res), core::Names::concat(), move(narg));
        res.reset(n.release());
    };
    return res;
}

unique_ptr<MethodDef> buildMethod(core::MutableContext ctx, core::Loc loc, core::NameRef name,
                                  unique_ptr<parser::Node> &argnode, unique_ptr<parser::Node> &body,
                                  u2 &uniqueCounter) {
    auto argsAndBody = desugarArgsAndBody(ctx, loc, argnode, body, uniqueCounter);
    return MK::Method(loc, name, move(argsAndBody.first), move(argsAndBody.second));
}

unique_ptr<Block> node2Proc(core::MutableContext ctx, unique_ptr<parser::Node> node, u2 &uniqueCounter) {
    if (node == nullptr) {
        return nullptr;
    }

    auto expr = node2TreeImpl(ctx, move(node), uniqueCounter);
    core::Loc loc = expr->loc;
    core::NameRef temp =
        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::blockPassTemp(), ++uniqueCounter);

    if (auto sym = cast_tree<SymbolLit>(expr.get())) {
        // &:foo => {|temp| temp.foo() }
        MethodDef::ARGS_store args;
        args.emplace_back(MK::Local(loc, temp));
        unique_ptr<Expression> recv = MK::Local(loc, temp);
        unique_ptr<Expression> body = MK::Send0(loc, move(recv), sym->name);
        return make_unique<Block>(loc, move(args), move(body));
    }

    // &foo => {|*args| foo.to_proc.call(*args) }
    auto proc = MK::Send0(loc, move(expr), core::Names::to_proc());
    MethodDef::ARGS_store args;
    unique_ptr<Expression> rest = make_unique<RestArg>(loc, MK::Local(loc, temp));
    args.emplace_back(move(rest));
    unique_ptr<Expression> body = MK::Send1(loc, move(proc), core::Names::call(), MK::Splat(loc, MK::Local(loc, temp)));
    return make_unique<Block>(loc, move(args), move(body));
}

unique_ptr<Expression> unsupportedNode(core::MutableContext ctx, parser::Node *node) {
    if (auto e = ctx.state.beginError(node->loc, core::errors::Desugar::UnsupportedNode)) {
        e.setHeader("Unsupported node type `{}`", node->nodeName());
    }
    return MK::EmptyTree(node->loc);
}

// Desugar a multi-assignment
//
// TODO(nelhage): Known incompletenesses:
//
//  - If the array is too small, and there are elements after a splat, we read
//    from the back of the array instead of padding with nil.
//
//     e.g. in `*b,c = x`, we assign `c = x[-1]`, even if x has only a single
//     element
//  - If `rhs` is not an array, we index into it anyways, instead of
//    `nil`-padding.
unique_ptr<Expression> desugarMlhs(core::MutableContext ctx, core::Loc loc, parser::Mlhs *lhs,
                                   unique_ptr<Expression> rhs, u2 &uniqueCounter) {
    InsSeq::STATS_store stats;

    core::NameRef tempName =
        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::assignTemp(), ++uniqueCounter);
    stats.emplace_back(MK::Assign(loc, tempName, move(rhs)));

    int i = 0;
    bool didSplat = false;

    for (auto &c : lhs->exprs) {
        if (auto *splat = parser::cast_node<parser::SplatLhs>(c.get())) {
            ENFORCE(!didSplat, "did splat already");
            didSplat = true;

            unique_ptr<Expression> lh = node2TreeImpl(ctx, move(splat->var), uniqueCounter);

            int left = i;
            int right = lhs->exprs.size() - left - 1;
            if (!isa_tree<EmptyTree>(lh.get())) {
                auto exclusive = MK::True(lh->loc);
                if (right == 0) {
                    right = 1;
                    exclusive = MK::False(lh->loc);
                }
                auto index =
                    MK::Send3(lh->loc, make_unique<Ident>(lh->loc, core::Symbols::Range()), core::Names::new_(),
                              MK::Int(lh->loc, left), MK::Int(lh->loc, -right), move(exclusive));
                stats.emplace_back(MK::Assign(
                    lh->loc, move(lh), MK::Send1(loc, MK::Local(loc, tempName), core::Names::slice(), move(index))));
            }
            i = -right;
        } else {
            auto val = MK::Send1(loc, MK::Local(loc, tempName), core::Names::squareBrackets(), MK::Int(loc, i));

            if (auto *mlhs = parser::cast_node<parser::Mlhs>(c.get())) {
                stats.emplace_back(desugarMlhs(ctx, mlhs->loc, mlhs, move(val), uniqueCounter));
            } else {
                unique_ptr<Expression> lh = node2TreeImpl(ctx, move(c), uniqueCounter);
                stats.emplace_back(MK::Assign(lh->loc, move(lh), move(val)));
            }

            i++;
        }
    }

    return MK::InsSeq(loc, move(stats), MK::Local(loc, tempName));
}

bool locReported = false;

ClassDef::RHS_store scopeNodeToBody(core::MutableContext ctx, unique_ptr<parser::Node> node) {
    ClassDef::RHS_store body;
    u2 uniqueCounter = 1;
    if (auto *begin = parser::cast_node<parser::Begin>(node.get())) {
        body.reserve(begin->stmts.size());
        for (auto &stat : begin->stmts) {
            body.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
        };
    } else {
        body.emplace_back(node2TreeImpl(ctx, move(node), uniqueCounter));
    }
    return body;
}

unique_ptr<Expression> node2TreeImpl(core::MutableContext ctx, unique_ptr<parser::Node> what, u2 &uniqueCounter) {
    try {
        if (what.get() == nullptr) {
            return MK::EmptyTree(core::Loc::none());
        }
        auto loc = what->loc;
        if (loc.is_none()) {
            ENFORCE("parse-tree node has no location: ", what->toString(ctx));
        }
        unique_ptr<Expression> result;
        typecase(
            what.get(),
            // The top N clauses here are ordered according to observed
            // frequency in pay-server. Do not reorder the top of this list, or
            // add entries here, without consulting the "node.*" counters from a
            // run over a representative code base.
            [&](parser::Send *send) {
                u4 flags = 0;
                auto rec = node2TreeImpl(ctx, move(send->receiver), uniqueCounter);
                if (isa_tree<EmptyTree>(rec.get())) {
                    rec = MK::Self(loc);
                    flags |= Send::PRIVATE_OK;
                }
                Send::ARGS_store args;
                unique_ptr<parser::Node> block;
                args.reserve(send->args.size());
                for (auto &stat : send->args) {
                    if (auto bp = parser::cast_node<parser::BlockPass>(stat.get())) {
                        ENFORCE(block == nullptr, "passing a block where there is no block");
                        block = move(bp->block);
                    } else {
                        args.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    }
                };

                auto res = MK::Send(loc, move(rec), send->method, move(args), flags,
                                    node2Proc(ctx, move(block), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Const *const_) {
                auto scope = node2TreeImpl(ctx, move(const_->scope), uniqueCounter);
                unique_ptr<Expression> res = MK::Constant(loc, move(scope), const_->name);
                result.swap(res);
            },
            [&](parser::String *string) {
                unique_ptr<Expression> res = make_unique<StringLit>(loc, string->val);
                result.swap(res);
            },
            [&](parser::Symbol *symbol) {
                unique_ptr<Expression> res = MK::Symbol(loc, symbol->val);
                result.swap(res);
            },
            [&](parser::LVar *var) {
                unique_ptr<Expression> res = MK::Local(loc, var->name);
                result.swap(res);
            },
            [&](parser::DString *dstring) {
                unique_ptr<Expression> res = desugarDString(ctx, loc, move(dstring->nodes), uniqueCounter);
                result.swap(res);
            },
            [&](parser::Begin *begin) {
                if (!begin->stmts.empty()) {
                    InsSeq::STATS_store stats;
                    stats.reserve(begin->stmts.size() - 1);
                    auto end = begin->stmts.end();
                    --end;
                    for (auto it = begin->stmts.begin(); it != end; ++it) {
                        auto &stat = *it;
                        stats.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    auto &last = begin->stmts.back();
                    auto expr = node2TreeImpl(ctx, move(last), uniqueCounter);
                    auto block = MK::InsSeq(loc, move(stats), move(expr));
                    result.swap(block);
                } else {
                    unique_ptr<Expression> res = MK::EmptyTree(loc);
                    result.swap(res);
                }
            },
            // END hand-ordered clauses
            [&](parser::And *and_) {
                auto lhs = node2TreeImpl(ctx, move(and_->left), uniqueCounter);
                if (auto i = cast_tree<Reference>(lhs.get())) {
                    auto cond = MK::cpRef(*i);
                    auto iff = MK::If(loc, move(cond), node2TreeImpl(ctx, move(and_->right), uniqueCounter), move(lhs));
                    result.swap(iff);
                } else {
                    core::NameRef tempName = ctx.state.freshNameUnique(core::UniqueNameKind::Desugar,
                                                                       core::Names::andAnd(), ++uniqueCounter);
                    auto temp = MK::Assign(loc, tempName, move(lhs));

                    auto iff = MK::If(loc, MK::Local(loc, tempName),
                                      node2TreeImpl(ctx, move(and_->right), uniqueCounter), MK::Local(loc, tempName));
                    auto wrapped = MK::InsSeq1(loc, move(temp), move(iff));

                    result.swap(wrapped);
                }
            },
            [&](parser::Or *or_) {
                auto lhs = node2TreeImpl(ctx, move(or_->left), uniqueCounter);
                if (auto i = cast_tree<Reference>(lhs.get())) {
                    auto cond = MK::cpRef(*i);
                    auto iff = MK::If(loc, move(cond), move(lhs), node2TreeImpl(ctx, move(or_->right), uniqueCounter));
                    result.swap(iff);
                } else {
                    core::NameRef tempName =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::orOr(), ++uniqueCounter);
                    auto temp = MK::Assign(loc, tempName, move(lhs));

                    auto iff = MK::If(loc, MK::Local(loc, tempName), MK::Local(loc, tempName),
                                      node2TreeImpl(ctx, move(or_->right), uniqueCounter));
                    auto wrapped = MK::InsSeq1(loc, move(temp), move(iff));

                    result.swap(wrapped);
                }
            },
            [&](parser::AndAsgn *andAsgn) {
                auto recv = node2TreeImpl(ctx, move(andAsgn->left), uniqueCounter);
                auto arg = node2TreeImpl(ctx, move(andAsgn->right), uniqueCounter);
                if (auto s = cast_tree<Send>(recv.get())) {
                    auto sendLoc = s->loc;
                    InsSeq::STATS_store stats;
                    core::NameRef tempRecv =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                    stats.emplace_back(MK::Assign(loc, tempRecv, move(s->recv)));
                    Send::ARGS_store readArgs;
                    Send::ARGS_store assgnArgs;
                    for (auto &arg : s->args) {
                        core::Loc loc = arg->loc;
                        core::NameRef name =
                            ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                        stats.emplace_back(MK::Assign(loc, name, move(arg)));
                        readArgs.emplace_back(MK::Local(loc, name));
                        assgnArgs.emplace_back(MK::Local(loc, name));
                    }
                    assgnArgs.emplace_back(move(arg));
                    auto cond = MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun, move(readArgs), s->flags);
                    core::NameRef tempResult =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                    stats.emplace_back(MK::Assign(loc, tempResult, move(cond)));

                    auto body =
                        MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun.addEq(ctx), move(assgnArgs), s->flags);
                    auto elsep = MK::Local(loc, tempResult);
                    auto iff = MK::If(loc, MK::Local(loc, tempResult), move(body), move(elsep));
                    auto wrapped = MK::InsSeq(loc, move(stats), move(iff));
                    result.swap(wrapped);
                } else if (auto i = cast_tree<Reference>(recv.get())) {
                    auto cond = MK::cpRef(*i);
                    auto body = MK::Assign(loc, move(recv), move(arg));
                    auto elsep = MK::cpRef(*i);
                    auto iff = MK::If(loc, move(cond), move(body), move(elsep));
                    result.swap(iff);
                } else {
                    Error::notImplemented();
                }
            },
            [&](parser::OrAsgn *orAsgn) {
                auto recv = node2TreeImpl(ctx, move(orAsgn->left), uniqueCounter);
                auto arg = node2TreeImpl(ctx, move(orAsgn->right), uniqueCounter);
                if (auto s = cast_tree<Send>(recv.get())) {
                    auto sendLoc = s->loc;
                    InsSeq::STATS_store stats;
                    core::NameRef tempRecv =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                    stats.emplace_back(MK::Assign(loc, tempRecv, move(s->recv)));
                    Send::ARGS_store readArgs;
                    Send::ARGS_store assgnArgs;
                    for (auto &arg : s->args) {
                        core::Loc loc = arg->loc;
                        core::NameRef name =
                            ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                        stats.emplace_back(MK::Assign(loc, name, move(arg)));
                        readArgs.emplace_back(MK::Local(loc, name));
                        assgnArgs.emplace_back(MK::Local(loc, name));
                    }
                    assgnArgs.emplace_back(move(arg));
                    auto cond = MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun, move(readArgs), s->flags);
                    core::NameRef tempResult =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                    stats.emplace_back(MK::Assign(loc, tempResult, move(cond)));

                    auto elsep =
                        MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun.addEq(ctx), move(assgnArgs), s->flags);
                    auto body = MK::Local(loc, tempResult);
                    auto iff = MK::If(loc, MK::Local(loc, tempResult), move(body), move(elsep));
                    auto wrapped = MK::InsSeq(loc, move(stats), move(iff));
                    result.swap(wrapped);
                } else if (auto i = cast_tree<Reference>(recv.get())) {
                    auto cond = MK::cpRef(*i);
                    auto body = MK::Assign(loc, move(recv), move(arg));
                    auto elsep = MK::cpRef(*i);
                    auto iff = MK::If(loc, move(cond), move(elsep), move(body));
                    result.swap(iff);

                } else {
                    Error::notImplemented();
                }
            },
            [&](parser::OpAsgn *opAsgn) {
                auto recv = node2TreeImpl(ctx, move(opAsgn->left), uniqueCounter);
                auto rhs = node2TreeImpl(ctx, move(opAsgn->right), uniqueCounter);
                if (auto s = cast_tree<Send>(recv.get())) {
                    auto sendLoc = s->loc;
                    InsSeq::STATS_store stats;
                    core::NameRef tempRecv =
                        ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                    stats.emplace_back(MK::Assign(loc, tempRecv, move(s->recv)));
                    Send::ARGS_store readArgs;
                    Send::ARGS_store assgnArgs;
                    for (auto &arg : s->args) {
                        core::Loc loc = arg->loc;
                        core::NameRef name =
                            ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, s->fun, ++uniqueCounter);
                        stats.emplace_back(MK::Assign(loc, name, move(arg)));
                        readArgs.emplace_back(MK::Local(loc, name));
                        assgnArgs.emplace_back(MK::Local(loc, name));
                    }
                    auto prevValue = MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun, move(readArgs), s->flags);
                    auto newValue = MK::Send1(loc, move(prevValue), opAsgn->op, move(rhs));
                    assgnArgs.emplace_back(move(newValue));

                    auto res =
                        MK::Send(sendLoc, MK::Local(loc, tempRecv), s->fun.addEq(ctx), move(assgnArgs), s->flags);
                    auto wrapped = MK::InsSeq(loc, move(stats), move(res));
                    result.swap(wrapped);
                } else if (auto i = cast_tree<Reference>(recv.get())) {
                    auto lhs = MK::cpRef(*i);
                    auto send = MK::Send1(loc, move(recv), opAsgn->op, move(rhs));
                    auto res = MK::Assign(loc, move(lhs), move(send));
                    result.swap(res);
                } else {
                    Error::notImplemented();
                }
            },
            [&](parser::CSend *csend) {
                core::NameRef tempRecv = ctx.state.freshNameUnique(core::UniqueNameKind::Desugar,
                                                                   core::Names::assignTemp(), ++uniqueCounter);
                core::Loc recvLoc = csend->receiver->loc;

                // NOTE(nelhage): We actually desugar into a call to `nil?`. If an
                // object has overridden `nil?`, this technically will not match
                // Ruby's behavior.

                auto assgn = MK::Assign(recvLoc, tempRecv, node2TreeImpl(ctx, move(csend->receiver), uniqueCounter));
                auto cond = MK::Send0(loc, MK::Local(recvLoc, tempRecv), core::Names::nil_p());

                unique_ptr<parser::Node> sendNode = make_unique<parser::Send>(
                    loc, make_unique<parser::LVar>(recvLoc, tempRecv), csend->method, move(csend->args));
                auto send = node2TreeImpl(ctx, move(sendNode), uniqueCounter);

                unique_ptr<Expression> nil = MK::Ident(loc, core::Symbols::nil());
                auto iff = MK::If(loc, move(cond), move(nil), move(send));
                InsSeq::STATS_store stats;
                stats.emplace_back(move(assgn));
                auto res = MK::InsSeq(loc, move(stats), move(iff));
                result.swap(res);
            },
            [&](parser::Self *self) {
                unique_ptr<Expression> res = MK::Self(loc);
                result.swap(res);
            },
            [&](parser::DSymbol *dsymbol) {
                if (dsymbol->nodes.empty()) {
                    unique_ptr<Expression> res = MK::Symbol(loc, core::Names::empty());
                    result.swap(res);
                    return;
                }

                auto it = dsymbol->nodes.begin();
                auto end = dsymbol->nodes.end();
                unique_ptr<Expression> res;
                unique_ptr<Expression> first = node2TreeImpl(ctx, move(*it), uniqueCounter);
                if (isa_tree<StringLit>(first.get())) {
                    res = move(first);
                } else {
                    res = MK::Send0(loc, move(first), core::Names::to_s());
                }
                ++it;
                for (; it != end; ++it) {
                    auto &stat = *it;
                    unique_ptr<Expression> narg = node2TreeImpl(ctx, move(stat), uniqueCounter);
                    if (!isa_tree<StringLit>(narg.get())) {
                        narg = MK::Send0(loc, move(narg), core::Names::to_s());
                    }
                    auto n = MK::Send1(loc, move(res), core::Names::concat(), move(narg));
                    res.reset(n.release());
                };
                res = MK::Send0(loc, move(res), core::Names::intern());

                result.swap(res);
            },
            [&](parser::FileLiteral *fileLiteral) {
                unique_ptr<Expression> res = make_unique<StringLit>(loc, core::Names::currentFile());
                result.swap(res);
            },
            [&](parser::ConstLhs *constLhs) {
                auto scope = node2TreeImpl(ctx, move(constLhs->scope), uniqueCounter);
                unique_ptr<Expression> res = MK::Constant(loc, move(scope), constLhs->name);
                result.swap(res);
            },
            [&](parser::Cbase *cbase) {
                unique_ptr<Expression> res = MK::Ident(loc, core::Symbols::root());
                result.swap(res);
            },
            [&](parser::Kwbegin *kwbegin) {
                if (!kwbegin->stmts.empty()) {
                    InsSeq::STATS_store stats;
                    stats.reserve(kwbegin->stmts.size() - 1);
                    auto end = kwbegin->stmts.end();
                    --end;
                    for (auto it = kwbegin->stmts.begin(); it != end; ++it) {
                        auto &stat = *it;
                        stats.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    auto &last = kwbegin->stmts.back();
                    auto expr = node2TreeImpl(ctx, move(last), uniqueCounter);
                    auto block = MK::InsSeq(loc, move(stats), move(expr));
                    result.swap(block);
                } else {
                    unique_ptr<Expression> res = MK::EmptyTree(loc);
                    result.swap(res);
                }
            },
            [&](parser::Module *module) {
                ClassDef::RHS_store body = scopeNodeToBody(ctx, move(module->body));
                ClassDef::ANCESTORS_store ancestors;
                unique_ptr<Expression> res = make_unique<ClassDef>(
                    loc, core::Symbols::todo(), node2TreeImpl(ctx, move(module->name), uniqueCounter), move(ancestors),
                    move(body), ClassDefKind::Module);
                result.swap(res);
            },
            [&](parser::Class *claz) {
                ClassDef::RHS_store body = scopeNodeToBody(ctx, move(claz->body));
                ClassDef::ANCESTORS_store ancestors;
                if (claz->superclass == nullptr) {
                    ancestors.emplace_back(make_unique<Ident>(loc, core::Symbols::todo()));
                } else {
                    ancestors.emplace_back(node2TreeImpl(ctx, move(claz->superclass), uniqueCounter));
                }

                unique_ptr<Expression> res = make_unique<ClassDef>(loc, core::Symbols::todo(),
                                                                   node2TreeImpl(ctx, move(claz->name), uniqueCounter),
                                                                   move(ancestors), move(body), ClassDefKind::Class);
                result.swap(res);
            },
            [&](parser::Arg *arg) {
                unique_ptr<Expression> res = MK::Local(loc, arg->name);
                result.swap(res);
            },
            [&](parser::Restarg *arg) {
                unique_ptr<Expression> res = make_unique<RestArg>(loc, MK::Local(loc, arg->name));
                result.swap(res);
            },
            [&](parser::Kwrestarg *arg) {
                unique_ptr<Expression> res =
                    make_unique<RestArg>(loc, make_unique<KeywordArg>(loc, MK::Local(loc, arg->name)));
                result.swap(res);
            },
            [&](parser::Kwarg *arg) {
                unique_ptr<Expression> res = make_unique<KeywordArg>(loc, MK::Local(loc, arg->name));
                result.swap(res);
            },
            [&](parser::Blockarg *arg) {
                unique_ptr<Expression> res = make_unique<BlockArg>(loc, MK::Local(loc, arg->name));
                result.swap(res);
            },
            [&](parser::Kwoptarg *arg) {
                unique_ptr<Expression> res =
                    make_unique<OptionalArg>(loc, make_unique<KeywordArg>(loc, MK::Local(loc, arg->name)),
                                             node2TreeImpl(ctx, move(arg->default_), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Optarg *arg) {
                unique_ptr<Expression> res = make_unique<OptionalArg>(
                    loc, MK::Local(loc, arg->name), node2TreeImpl(ctx, move(arg->default_), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Shadowarg *arg) {
                unique_ptr<Expression> res = make_unique<ShadowArg>(loc, MK::Local(loc, arg->name));
                result.swap(res);
            },
            [&](parser::DefMethod *method) {
                u2 uniqueCounter1 = 1;
                unique_ptr<Expression> res =
                    buildMethod(ctx, loc, method->name, method->args, method->body, uniqueCounter1);
                result.swap(res);
            },
            [&](parser::DefS *method) {
                parser::Self *self = parser::cast_node<parser::Self>(method->singleton.get());
                if (self == nullptr) {
                    if (auto e = ctx.state.beginError(loc, core::errors::Desugar::InvalidSingletonDef)) {
                        e.setHeader("`def EXPRESSION.method' is only supported for `def self.method'");
                    }
                    unique_ptr<Expression> res = MK::EmptyTree(loc);
                    result.swap(res);
                    return;
                }
                u2 uniqueCounter1 = 1;
                unique_ptr<MethodDef> meth =
                    buildMethod(ctx, loc, method->name, method->args, method->body, uniqueCounter1);
                meth->isSelf = true;
                unique_ptr<Expression> res(meth.release());
                result.swap(res);
            },
            [&](parser::SClass *sclass) {
                // This will be a nested ClassDef which we leave in the tree
                // which will get the symbol of `class.singleton_class`
                parser::Self *self = parser::cast_node<parser::Self>(sclass->expr.get());
                if (self == nullptr) {
                    if (auto e = ctx.state.beginError(sclass->loc, core::errors::Desugar::InvalidSingletonDef)) {
                        e.setHeader("`class << EXPRESSION' is only supported for `class << self'");
                    }
                    unique_ptr<Expression> res = MK::EmptyTree(what->loc);
                    result.swap(res);
                    return;
                }

                ClassDef::RHS_store body = scopeNodeToBody(ctx, move(sclass->body));
                ClassDef::ANCESTORS_store emptyAncestors;
                unique_ptr<Expression> res = make_unique<ClassDef>(
                    what->loc, core::Symbols::todo(),
                    make_unique<UnresolvedIdent>(sclass->expr->loc, UnresolvedIdent::Class, core::Names::singleton()),
                    move(emptyAncestors), move(body), ClassDefKind::Class);
                result.swap(res);
            },
            [&](parser::Block *block) {
                auto recv = node2TreeImpl(ctx, move(block->send), uniqueCounter);
                Send *send;
                unique_ptr<Expression> res;
                if ((send = cast_tree<Send>(recv.get())) != nullptr) {
                    res.swap(recv);
                } else {
                    // This must have been a csend; That will have been desugared
                    // into an insseq with an If in the expression.
                    res.swap(recv);
                    InsSeq *is = cast_tree<InsSeq>(res.get());
                    ENFORCE(is != nullptr, "DesugarBlock: failed to find InsSeq");
                    If *iff = cast_tree<If>(is->expr.get());
                    ENFORCE(iff != nullptr, "DesugarBlock: failed to find If");
                    send = cast_tree<Send>(iff->elsep.get());
                    ENFORCE(send != nullptr, "DesugarBlock: failed to find Send");
                }
                auto argsAndBody = desugarArgsAndBody(ctx, loc, block->args, block->body, uniqueCounter);

                send->block = make_unique<Block>(loc, move(argsAndBody.first), move(argsAndBody.second));
                result.swap(res);
            },
            [&](parser::While *wl) {
                auto cond = node2TreeImpl(ctx, move(wl->cond), uniqueCounter);
                auto body = node2TreeImpl(ctx, move(wl->body), uniqueCounter);
                unique_ptr<Expression> res = make_unique<While>(loc, move(cond), move(body));
                result.swap(res);
            },
            [&](parser::WhilePost *wl) {
                auto cond = node2TreeImpl(ctx, move(wl->cond), uniqueCounter);
                auto body = node2TreeImpl(ctx, move(wl->body), uniqueCounter);
                unique_ptr<Expression> res = make_unique<While>(loc, move(cond), move(body));
                result.swap(res);
            },
            [&](parser::Until *wl) {
                auto cond = MK::Send0(loc, node2TreeImpl(ctx, move(wl->cond), uniqueCounter), core::Names::bang());
                auto body = node2TreeImpl(ctx, move(wl->body), uniqueCounter);
                unique_ptr<Expression> res = make_unique<While>(loc, move(cond), move(body));
                result.swap(res);
            },
            [&](parser::UntilPost *wl) {
                auto cond = MK::Send0(loc, node2TreeImpl(ctx, move(wl->cond), uniqueCounter), core::Names::bang());
                auto body = node2TreeImpl(ctx, move(wl->body), uniqueCounter);
                unique_ptr<Expression> res = make_unique<While>(loc, move(cond), move(body));
                result.swap(res);
            },
            [&](parser::Nil *wl) {
                unique_ptr<Expression> res = MK::Ident(loc, core::Symbols::nil());
                result.swap(res);
            },
            [&](parser::IVar *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Instance, var->name);
                result.swap(res);
            },
            [&](parser::GVar *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Global, var->name);
                result.swap(res);
            },
            [&](parser::CVar *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Class, var->name);
                result.swap(res);
            },
            [&](parser::LVarLhs *var) {
                unique_ptr<Expression> res = MK::Local(loc, var->name);
                result.swap(res);
            },
            [&](parser::GVarLhs *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Global, var->name);
                result.swap(res);
            },
            [&](parser::CVarLhs *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Class, var->name);
                result.swap(res);
            },
            [&](parser::IVarLhs *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Instance, var->name);
                result.swap(res);
            },
            [&](parser::NthRef *var) {
                unique_ptr<Expression> res = make_unique<UnresolvedIdent>(loc, UnresolvedIdent::Global,
                                                                          ctx.state.enterNameUTF8(to_string(var->ref)));
                result.swap(res);
            },
            [&](parser::Assign *asgn) {
                auto lhs = node2TreeImpl(ctx, move(asgn->lhs), uniqueCounter);
                auto rhs = node2TreeImpl(ctx, move(asgn->rhs), uniqueCounter);
                auto res = MK::Assign(loc, move(lhs), move(rhs));
                result.swap(res);
            },
            [&](parser::Super *super) {
                auto method = core::Names::super();

                Send::ARGS_store args;
                unique_ptr<parser::Node> block;
                args.reserve(super->args.size());
                for (auto &stat : super->args) {
                    if (auto bp = parser::cast_node<parser::BlockPass>(stat.get())) {
                        ENFORCE(block == nullptr, "No Block in super blockpass");
                        block = move(bp->block);
                    } else {
                        args.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    }
                };

                unique_ptr<Expression> res =
                    MK::Send(loc, MK::Self(loc), method, move(args), 0, node2Proc(ctx, move(block), uniqueCounter));

                result.swap(res);
            },
            [&](parser::ZSuper *zuper) {
                unique_ptr<Expression> res =
                    MK::Send1(loc, MK::Self(loc), core::Names::super(), make_unique<ZSuperArgs>(zuper->loc));
                result.swap(res);
            },
            [&](parser::For *for_) {
                auto temp =
                    ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::forTemp(), ++uniqueCounter);

                auto mlhsNode = move(for_->vars);
                if (!parser::isa_node<parser::Mlhs>(mlhsNode.get())) {
                    parser::NodeVec vars;
                    vars.emplace_back(move(mlhsNode));
                    mlhsNode = make_unique<parser::Mlhs>(loc, move(vars));
                }
                unique_ptr<parser::Node> masgn =
                    make_unique<parser::Masgn>(loc, move(mlhsNode), make_unique<parser::LVar>(loc, temp));

                InsSeq::STATS_store stats;
                stats.emplace_back(node2TreeImpl(ctx, move(masgn), uniqueCounter));
                auto body = make_unique<InsSeq>(loc, move(stats), node2TreeImpl(ctx, move(for_->body), uniqueCounter));

                MethodDef::ARGS_store blockArgs;
                blockArgs.emplace_back(make_unique<RestArg>(loc, MK::Local(loc, temp)));
                auto block = make_unique<Block>(loc, move(blockArgs), move(body));

                Send::ARGS_store noargs;
                auto res = MK::Send(loc, node2TreeImpl(ctx, move(for_->expr), uniqueCounter), core::Names::each(),
                                    move(noargs), 0, move(block));
                result.swap(res);
            },
            [&](parser::Integer *integer) {
                int64_t val;
                try {
                    val = stol(integer->val);
                } catch (std::out_of_range &) {
                    val = 0;
                    if (auto e = ctx.state.beginError(loc, core::errors::Desugar::IntegerOutOfRange)) {
                        e.setHeader("Unsupported large integer literal: `{}`", integer->val);
                    }
                } catch (std::invalid_argument &) {
                    val = 0;
                    if (auto e = ctx.state.beginError(loc, core::errors::Desugar::IntegerOutOfRange)) {
                        e.setHeader("Unsupported integer literal: `{}`", integer->val);
                    }
                }

                unique_ptr<Expression> res = MK::Int(loc, val);
                result.swap(res);
            },
            [&](parser::Float *floatNode) {
                double val;
                try {
                    val = stod(floatNode->val);
                    if (isinf(val)) {
                        val = std::numeric_limits<double>::quiet_NaN();
                        if (auto e = ctx.state.beginError(loc, core::errors::Desugar::FloatOutOfRange)) {
                            e.setHeader("Unsupported large float literal: `{}`", floatNode->val);
                        }
                    }
                } catch (std::out_of_range &) {
                    val = std::numeric_limits<double>::quiet_NaN();
                    if (auto e = ctx.state.beginError(loc, core::errors::Desugar::FloatOutOfRange)) {
                        e.setHeader("Unsupported large float literal: `{}`", floatNode->val);
                    }
                } catch (std::invalid_argument &) {
                    val = std::numeric_limits<double>::quiet_NaN();
                    if (auto e = ctx.state.beginError(loc, core::errors::Desugar::FloatOutOfRange)) {
                        e.setHeader("Unsupported float literal: `{}`", floatNode->val);
                    }
                }

                unique_ptr<Expression> res = make_unique<FloatLit>(loc, val);
                result.swap(res);
            },
            [&](parser::Complex *complex) {
                auto kernel = MK::Ident(loc, core::Symbols::Kernel());
                core::NameRef complex_name = core::Symbols::Complex().data(ctx).name;
                core::NameRef value = ctx.state.enterNameUTF8(complex->value);
                auto send = MK::Send1(loc, move(kernel), complex_name, make_unique<StringLit>(loc, value));
                result.swap(send);
            },
            [&](parser::Rational *complex) {
                auto kernel = MK::Ident(loc, core::Symbols::Kernel());
                core::NameRef complex_name = core::Symbols::Rational().data(ctx).name;
                core::NameRef value = ctx.state.enterNameUTF8(complex->val);
                auto send = MK::Send1(loc, move(kernel), complex_name, make_unique<StringLit>(loc, value));
                result.swap(send);
            },
            [&](parser::Array *array) {
                Array::ENTRY_store elems;
                elems.reserve(array->elts.size());
                unique_ptr<Expression> lastMerge;
                for (auto &stat : array->elts) {
                    if (auto splat = parser::cast_node<parser::Splat>(stat.get())) {
                        // Desguar
                        //   [a, **x, remaining}
                        // into
                        //   a.concat(x.to_a).concat(remaining)
                        auto var =
                            MK::Send0(loc, node2TreeImpl(ctx, move(splat->var), uniqueCounter), core::Names::to_a());
                        if (elems.empty()) {
                            if (lastMerge != nullptr) {
                                lastMerge = MK::Send1(loc, move(lastMerge), core::Names::concat(), move(var));
                            } else {
                                lastMerge = move(var);
                            }
                        } else {
                            unique_ptr<Expression> current = make_unique<Array>(loc, move(elems));
                            elems.clear();
                            if (lastMerge != nullptr) {
                                lastMerge = MK::Send1(loc, move(lastMerge), core::Names::concat(), move(current));
                            } else {
                                lastMerge = move(current);
                            }
                            lastMerge = MK::Send1(loc, move(lastMerge), core::Names::concat(), move(var));
                        }
                    } else {
                        elems.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    }
                };

                unique_ptr<Expression> res;
                if (elems.empty()) {
                    if (lastMerge != nullptr) {
                        res = move(lastMerge);
                    } else {
                        // Empty array
                        res = make_unique<Array>(loc, move(elems));
                    }
                } else {
                    res = make_unique<Array>(loc, move(elems));
                    if (lastMerge != nullptr) {
                        res = MK::Send1(loc, move(lastMerge), core::Names::concat(), move(res));
                    }
                }
                result.swap(res);
            },
            [&](parser::Hash *hash) {
                Hash::ENTRY_store keys;
                Hash::ENTRY_store values;
                keys.reserve(hash->pairs.size());   // overapproximation in case there are KwSpats
                values.reserve(hash->pairs.size()); // overapproximation in case there are KwSpats
                unique_ptr<Expression> lastMerge;

                for (auto &pairAsExpression : hash->pairs) {
                    parser::Pair *pair = parser::cast_node<parser::Pair>(pairAsExpression.get());
                    if (pair != nullptr) {
                        auto key = node2TreeImpl(ctx, move(pair->key), uniqueCounter);
                        auto value = node2TreeImpl(ctx, move(pair->value), uniqueCounter);
                        keys.emplace_back(move(key));
                        values.emplace_back(move(value));
                    } else {
                        parser::Kwsplat *splat = parser::cast_node<parser::Kwsplat>(pairAsExpression.get());
                        ENFORCE(splat != nullptr, "kwsplat cast failed");

                        // Desguar
                        //   {a: 'a', **x, remaining}
                        // into
                        //   {a: 'a'}.merge(x.to_h).merge(remaining)
                        auto expr = MK::Send0(loc, node2TreeImpl(ctx, move(splat->expr), uniqueCounter),
                                              core::Names::to_hash());
                        if (keys.empty()) {
                            if (lastMerge != nullptr) {
                                lastMerge = MK::Send1(loc, move(lastMerge), core::Names::merge(), move(expr));

                            } else {
                                lastMerge = move(expr);
                            }
                        } else {
                            unique_ptr<Expression> current = make_unique<Hash>(loc, move(keys), move(values));
                            keys.clear();
                            values.clear();
                            if (lastMerge != nullptr) {
                                lastMerge = MK::Send1(loc, move(lastMerge), core::Names::merge(), move(current));
                            } else {
                                lastMerge = move(current);
                            }
                            lastMerge = MK::Send1(loc, move(lastMerge), core::Names::merge(), move(expr));
                        }
                    }
                };

                unique_ptr<Expression> res;
                if (keys.empty()) {
                    if (lastMerge != nullptr) {
                        res = move(lastMerge);
                    } else {
                        // Empty array
                        res = make_unique<Hash>(loc, move(keys), move(values));
                    }
                } else {
                    res = make_unique<Hash>(loc, move(keys), move(values));
                    if (lastMerge != nullptr) {
                        res = MK::Send1(loc, move(lastMerge), core::Names::merge(), move(res));
                    }
                }

                result.swap(res);
            },
            [&](parser::IRange *ret) {
                core::NameRef range_name = core::Symbols::Range().data(ctx).name;
                unique_ptr<Expression> range = MK::Constant(loc, MK::EmptyTree(loc), range_name);
                auto from = node2TreeImpl(ctx, move(ret->from), uniqueCounter);
                auto to = node2TreeImpl(ctx, move(ret->to), uniqueCounter);
                auto send = MK::Send2(loc, move(range), core::Names::new_(), move(from), move(to));
                result.swap(send);
            },
            [&](parser::ERange *ret) {
                unique_ptr<Expression> range = MK::Ident(loc, core::Symbols::Range());
                auto from = node2TreeImpl(ctx, move(ret->from), uniqueCounter);
                auto to = node2TreeImpl(ctx, move(ret->to), uniqueCounter);
                auto true_ = MK::True(loc);
                auto send = MK::Send3(loc, move(range), core::Names::new_(), move(from), move(to), move(true_));
                result.swap(send);
            },
            [&](parser::Regexp *regexpNode) {
                unique_ptr<Expression> regexp = MK::Ident(loc, core::Symbols::Regexp());
                auto regex = desugarDString(ctx, loc, move(regexpNode->regex), uniqueCounter);
                auto opts = node2TreeImpl(ctx, move(regexpNode->opts), uniqueCounter);
                auto send = MK::Send2(loc, move(regexp), core::Names::new_(), move(regex), move(opts));
                result.swap(send);
            },
            [&](parser::Regopt *regopt) {
                unique_ptr<Expression> acc = MK::Int(loc, 0);
                for (auto &chr : regopt->opts) {
                    int flag = 0;
                    switch (chr) {
                        case 'i':
                            flag = 1; // Regexp::IGNORECASE
                            break;
                        case 'x':
                            flag = 2; // Regexp::EXTENDED
                            break;
                        case 'm':
                            flag = 4; // Regexp::MULILINE
                            break;
                        case 'n':
                        case 'e':
                        case 's':
                        case 'u':
                            // Encoding options that should already be handled by the parser
                            break;
                        default:
                            // The parser already yelled about this
                            break;
                    }
                    if (flag != 0) {
                        acc = MK::Send1(loc, move(acc), core::Names::orOp(), MK::Int(loc, flag));
                    }
                }
                result.swap(acc);
            },
            [&](parser::Return *ret) {
                if (ret->exprs.size() > 1) {
                    Array::ENTRY_store elems;
                    elems.reserve(ret->exprs.size());
                    for (auto &stat : ret->exprs) {
                        elems.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    unique_ptr<Expression> arr = make_unique<Array>(loc, move(elems));
                    unique_ptr<Expression> res = make_unique<Return>(loc, move(arr));
                    result.swap(res);
                } else if (ret->exprs.size() == 1) {
                    unique_ptr<Expression> res =
                        make_unique<Return>(loc, node2TreeImpl(ctx, move(ret->exprs[0]), uniqueCounter));
                    result.swap(res);
                } else {
                    unique_ptr<Expression> res = make_unique<Return>(loc, MK::EmptyTree(loc));
                    result.swap(res);
                }
            },
            [&](parser::Break *ret) {
                if (ret->exprs.size() > 1) {
                    Array::ENTRY_store elems;
                    elems.reserve(ret->exprs.size());
                    for (auto &stat : ret->exprs) {
                        elems.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    unique_ptr<Expression> arr = make_unique<Array>(loc, move(elems));
                    unique_ptr<Expression> res = make_unique<Break>(loc, move(arr));
                    result.swap(res);
                } else if (ret->exprs.size() == 1) {
                    unique_ptr<Expression> res =
                        make_unique<Break>(loc, node2TreeImpl(ctx, move(ret->exprs[0]), uniqueCounter));
                    result.swap(res);
                } else {
                    unique_ptr<Expression> res = make_unique<Break>(loc, MK::EmptyTree(loc));
                    result.swap(res);
                }
            },
            [&](parser::Next *ret) {
                if (ret->exprs.size() > 1) {
                    Array::ENTRY_store elems;
                    elems.reserve(ret->exprs.size());
                    for (auto &stat : ret->exprs) {
                        elems.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    unique_ptr<Expression> arr = make_unique<Array>(loc, move(elems));
                    unique_ptr<Expression> res = make_unique<Next>(loc, move(arr));
                    result.swap(res);
                } else if (ret->exprs.size() == 1) {
                    unique_ptr<Expression> res =
                        make_unique<Next>(loc, node2TreeImpl(ctx, move(ret->exprs[0]), uniqueCounter));
                    result.swap(res);
                } else {
                    unique_ptr<Expression> res = make_unique<Next>(loc, MK::EmptyTree(loc));
                    result.swap(res);
                }
            },
            [&](parser::Retry *ret) {
                unique_ptr<Expression> res = make_unique<Retry>(loc);
                result.swap(res);
            },
            [&](parser::Yield *ret) {
                if (ret->exprs.size() > 1) {
                    Array::ENTRY_store elems;
                    elems.reserve(ret->exprs.size());
                    for (auto &stat : ret->exprs) {
                        elems.emplace_back(node2TreeImpl(ctx, move(stat), uniqueCounter));
                    };
                    unique_ptr<Expression> arr = make_unique<Array>(loc, move(elems));
                    unique_ptr<Expression> res = make_unique<Yield>(loc, move(arr));
                    result.swap(res);
                } else if (ret->exprs.size() == 1) {
                    unique_ptr<Expression> res =
                        make_unique<Yield>(loc, node2TreeImpl(ctx, move(ret->exprs[0]), uniqueCounter));
                    result.swap(res);
                } else {
                    unique_ptr<Expression> res = make_unique<Yield>(loc, MK::EmptyTree(loc));
                    result.swap(res);
                }
            },
            [&](parser::Rescue *rescue) {
                Rescue::RESCUE_CASE_store cases;
                for (auto &node : rescue->rescue) {
                    unique_ptr<Expression> rescueCaseExpr = node2TreeImpl(ctx, move(node), uniqueCounter);
                    auto rescueCase = cast_tree<ast::RescueCase>(rescueCaseExpr.get());
                    ENFORCE(rescueCase != nullptr, "rescue case cast failed");
                    cases.emplace_back(rescueCase);
                    rescueCaseExpr.release();
                }
                unique_ptr<Expression> res =
                    make_unique<Rescue>(loc, node2TreeImpl(ctx, move(rescue->body), uniqueCounter), move(cases),
                                        node2TreeImpl(ctx, move(rescue->else_), uniqueCounter), MK::EmptyTree(loc));
                result.swap(res);
            },
            [&](parser::Resbody *resbody) {
                RescueCase::EXCEPTION_store exceptions;
                auto exceptionsExpr = node2TreeImpl(ctx, move(resbody->exception), uniqueCounter);
                if (isa_tree<EmptyTree>(exceptionsExpr.get())) {
                    // No exceptions captured
                } else if (auto exceptionsArray = cast_tree<ast::Array>(exceptionsExpr.get())) {
                    ENFORCE(exceptionsArray != nullptr, "exception array cast failed");

                    for (auto &elem : exceptionsArray->elems) {
                        exceptions.emplace_back(move(elem));
                    }
                } else if (auto exceptionsSend = cast_tree<ast::Send>(exceptionsExpr.get())) {
                    ENFORCE(exceptionsSend->fun == core::Names::splat() || exceptionsSend->fun == core::Names::to_a() ||
                                exceptionsSend->fun == core::Names::concat(),
                            "Unknown exceptionSend function");
                    exceptions.emplace_back(move(exceptionsExpr));
                } else {
                    Error::raise("Bad inner node type");
                }

                auto varExpr = node2TreeImpl(ctx, move(resbody->var), uniqueCounter);
                auto body = node2TreeImpl(ctx, move(resbody->body), uniqueCounter);

                auto varLoc = varExpr->loc;
                auto var = core::NameRef::noName();
                if (auto *id = cast_tree<UnresolvedIdent>(varExpr.get())) {
                    if (id->kind == UnresolvedIdent::Local) {
                        var = id->name;
                        varExpr.reset(nullptr);
                    }
                }

                if (!var.exists()) {
                    var = ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::rescueTemp(),
                                                    ++uniqueCounter);
                }

                if (isa_tree<EmptyTree>(varExpr.get())) {
                    varLoc = loc;
                } else if (varExpr != nullptr) {
                    body = MK::InsSeq1(varLoc, MK::Assign(varLoc, move(varExpr), MK::Local(varLoc, var)), move(body));
                }

                unique_ptr<Expression> res =
                    make_unique<RescueCase>(loc, move(exceptions), MK::Local(varLoc, var), move(body));
                result.swap(res);
            },
            [&](parser::Ensure *ensure) {
                auto bodyExpr = node2TreeImpl(ctx, move(ensure->body), uniqueCounter);
                auto ensureExpr = node2TreeImpl(ctx, move(ensure->ensure), uniqueCounter);
                auto rescue = cast_tree<ast::Rescue>(bodyExpr.get());
                if (rescue != nullptr) {
                    rescue->ensure = move(ensureExpr);
                    result.swap(bodyExpr);
                } else {
                    Rescue::RESCUE_CASE_store cases;
                    unique_ptr<Expression> res =
                        make_unique<Rescue>(loc, move(bodyExpr), move(cases), MK::EmptyTree(loc), move(ensureExpr));
                    result.swap(res);
                }
            },
            [&](parser::If *if_) {
                auto cond = node2TreeImpl(ctx, move(if_->condition), uniqueCounter);
                auto thenp = node2TreeImpl(ctx, move(if_->then_), uniqueCounter);
                auto elsep = node2TreeImpl(ctx, move(if_->else_), uniqueCounter);
                auto iff = MK::If(loc, move(cond), move(thenp), move(elsep));
                result.swap(iff);
            },
            [&](parser::Masgn *masgn) {
                parser::Mlhs *lhs = parser::cast_node<parser::Mlhs>(masgn->lhs.get());
                ENFORCE(lhs != nullptr, "Failed to get lhs of Masgn");

                auto res =
                    desugarMlhs(ctx, loc, lhs, node2TreeImpl(ctx, move(masgn->rhs), uniqueCounter), uniqueCounter);

                result.swap(res);
            },
            [&](parser::True *t) {
                auto res = MK::True(loc);
                result.swap(res);
            },
            [&](parser::False *t) {
                auto res = MK::False(loc);
                result.swap(res);
            },
            [&](parser::Case *case_) {
                unique_ptr<Expression> assign;
                auto temp = core::NameRef::noName();
                core::Loc cloc;

                if (case_->condition != nullptr) {
                    cloc = case_->condition->loc;
                    temp = ctx.state.freshNameUnique(core::UniqueNameKind::Desugar, core::Names::assignTemp(),
                                                     ++uniqueCounter);
                    assign = MK::Assign(cloc, temp, node2TreeImpl(ctx, move(case_->condition), uniqueCounter));
                }
                unique_ptr<Expression> res = node2TreeImpl(ctx, move(case_->else_), uniqueCounter);
                for (auto it = case_->whens.rbegin(); it != case_->whens.rend(); ++it) {
                    auto when = parser::cast_node<parser::When>(it->get());
                    ENFORCE(when != nullptr, "case without a when?");
                    unique_ptr<Expression> cond;
                    for (auto &cnode : when->patterns) {
                        auto ctree = node2TreeImpl(ctx, move(cnode), uniqueCounter);
                        unique_ptr<Expression> test;
                        if (temp.exists()) {
                            auto local = MK::Local(cloc, temp);
                            test = MK::Send1(ctree->loc, move(ctree), core::Names::tripleEq(), move(local));
                        } else {
                            test.swap(ctree);
                        }
                        if (cond == nullptr) {
                            cond.swap(test);
                        } else {
                            auto true_ = MK::True(test->loc);
                            auto loc = test->loc;
                            cond = MK::If(loc, move(test), move(true_), move(cond));
                        }
                    }
                    res = MK::If(when->loc, move(cond), node2TreeImpl(ctx, move(when->body), uniqueCounter), move(res));
                }
                if (assign != nullptr) {
                    res = MK::InsSeq1(loc, move(assign), move(res));
                }
                result.swap(res);
            },
            [&](parser::Splat *splat) {
                auto res = MK::Splat(loc, node2TreeImpl(ctx, move(splat->var), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Alias *alias) {
                auto res = MK::Send2(loc, MK::Self(loc), core::Names::aliasMethod(),
                                     node2TreeImpl(ctx, move(alias->from), uniqueCounter),
                                     node2TreeImpl(ctx, move(alias->to), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Defined *defined) {
                // TODO: if defined->value isn't defined, with this
                // implementation we will still raise an undefined error
                auto res = MK::Send1(loc, MK::Self(loc), core::Names::defined_p(),
                                     node2TreeImpl(ctx, move(defined->value), uniqueCounter));
                result.swap(res);
            },
            [&](parser::LineLiteral *line) {
                auto pos = loc.position(ctx);
                ENFORCE(pos.first.line == pos.second.line, "position corrupted");
                auto res = MK::Int(loc, pos.first.line);
                result.swap(res);
            },
            [&](parser::XString *xstring) {
                auto res = MK::Send1(loc, MK::Self(loc), core::Names::backtick(),
                                     desugarDString(ctx, loc, move(xstring->nodes), uniqueCounter));
                result.swap(res);
            },
            [&](parser::Preexe *preexe) {
                auto res = unsupportedNode(ctx, preexe);
                result.swap(res);
            },
            [&](parser::Postexe *postexe) {
                auto res = unsupportedNode(ctx, postexe);
                result.swap(res);
            },
            [&](parser::Undef *undef) {
                auto res = unsupportedNode(ctx, undef);
                result.swap(res);
            },
            [&](parser::Backref *backref) {
                auto res = unsupportedNode(ctx, backref);
                result.swap(res);
            },
            [&](parser::EFlipflop *eflipflop) {
                auto res = unsupportedNode(ctx, eflipflop);
                result.swap(res);
            },
            [&](parser::IFlipflop *iflipflop) {
                auto res = unsupportedNode(ctx, iflipflop);
                result.swap(res);
            },
            [&](parser::MatchCurLine *matchCurLine) {
                auto res = unsupportedNode(ctx, matchCurLine);
                result.swap(res);
            },
            [&](parser::Redo *redo) {
                auto res = unsupportedNode(ctx, redo);
                result.swap(res);
            },

            [&](parser::BlockPass *blockPass) { Error::raise("Send should have already handled the BlockPass"); },
            [&](parser::Node *node) { Error::raise("Unimplemented Parser Node: ", node->nodeName()); });
        ENFORCE(result.get() != nullptr, "desugar result unset");
        return result;
    } catch (...) {
        if (!locReported) {
            locReported = true;
            if (auto e = ctx.state.beginError(what->loc, core::errors::Internal::InternalError)) {
                e.setHeader("Failed to process tree (backtrace is above)");
            }
        }
        throw;
    }
}

unique_ptr<Expression> liftTopLevel(core::MutableContext ctx, unique_ptr<Expression> what) {
    if (isa_tree<ClassDef>(what.get())) {
        return what;
    }

    auto loc = what->loc;

    ClassDef::RHS_store rhs;
    rhs.emplace_back(move(what));
    return make_unique<ClassDef>(loc, core::Symbols::root(), MK::EmptyTree(core::Loc::none()),
                                 ClassDef::ANCESTORS_store(), move(rhs), Class);
}
} // namespace

unique_ptr<Expression> node2Tree(core::MutableContext ctx, unique_ptr<parser::Node> what) {
    try {
        u2 uniqueCounter = 1;
        auto result = node2TreeImpl(ctx, move(what), uniqueCounter);
        result = liftTopLevel(ctx, move(result));
        auto verifiedResult = Verifier::run(ctx, move(result));
        return verifiedResult;
    } catch (...) {
        locReported = false;
        throw;
    }
}
} // namespace desugar
} // namespace ast
} // namespace ruby_typer
