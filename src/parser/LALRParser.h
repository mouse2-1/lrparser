#ifndef LRPARSER_LALR_H
#define LRPARSER_LALR_H

#include "src/automata/Automaton.h"
#include "src/common.h"
#include "src/grammar/Grammar.h"
#include "src/parser/LRParser.h"
#include "src/util/BitSet.h"
#include <cassert>
#include <cstddef>
#include <optional>
#include <queue>
#include <stack>
#include <unordered_set>
#include <utility>

namespace gram {
class LALRParser : public LRParser {
  private:
    using Constraint = util::BitSet<ActionID>;
    using LALRClosure = std::map<StateID, Constraint>;

    void makeClosure(std::vector<State> const &lr0States,
                     LALRClosure &unclosured) {
        ActionID epsilonID = gram.getEpsilonSymbol().id;
        std::stack<decltype(unclosured.begin())> stack;
        for (auto it = unclosured.begin(); it != unclosured.end(); ++it) {
            stack.push(it);
        }
        while (!stack.empty()) {
            auto lalrStateIter = stack.top();
            stack.pop();
            auto const &lr0State = lr0States[lalrStateIter->first];
            auto const &trans = *lr0State.transitions;
            auto range = trans.rangeOf(epsilonID);
            for (auto it = range.first; it != range.second; ++it) {
                auto iter = unclosured.find(it->destination);
                if (iter == unclosured.end()) {
                    // State's not in closure. Should add it to closure.
                    auto result = unclosured.emplace(
                        it->destination,
                        resolveConstraintsPrivate(&lalrStateIter->second,
                                                  lr0State.productionID,
                                                  lr0State.rhsIndex));
                    stack.push(result.first);
                } else {
                    // Update constraint.
                    iter->second |= resolveConstraintsPrivate(
                        &lalrStateIter->second, lr0State.productionID,
                        lr0State.rhsIndex);
                }
            }
        }
    }

    // actionID cannot be epsilonID
    std::map<StateID, Constraint> transit(std::vector<State> const &lr0States,
                                          ActionID actionID,
                                          LALRClosure const &lalrClosure) {
        std::map<StateID, Constraint> res;

        for (auto const &[lr0StateID, constraint] : lalrClosure) {
            auto const &lr0State = lr0States[lr0StateID];
            auto const &trans = *lr0State.transitions;
            auto range = trans.rangeOf(actionID);
            for (auto it = range.first; it != range.second; ++it) {
                auto iter = res.find(it->destination);
                if (iter == res.end()) {
                    res.emplace(it->destination, constraint);
                } else {
                    // Must merge
                    iter->second |= constraint;
                }
            }
        }

        makeClosure(lr0States, res);
        return res;
    }

    // Real constraint resolving method.
    [[nodiscard]] Constraint
    resolveConstraintsPrivate(const Constraint *parentConstraint,
                              ProductionID prodID, int rhsIndex) {
        auto const &symbols = gram.getAllSymbols();
        Constraint constraint(symbols.size());

        // Handle "S' -> S" carefully.
        auto const &productionTable = gram.getProductionTable();
        if (prodID == productionTable.size()) {
            constraint.insert(gram.getEndOfInputSymbol().id);
            return constraint;
        }

        auto const &rhs = productionTable[prodID].rightSymbols;

        bool allNullable = true;
        for (size_t i = rhsIndex + 1; allNullable && i < rhs.size(); ++i) {
            if (!symbols[rhs[i]].nullable.value())
                allNullable = false;
            constraint |= symbols[rhs[i]].firstSet;
        }
        if (allNullable && parentConstraint)
            constraint |= *parentConstraint;

        return constraint;
    }

    // Ignore constraints and only compare kernels
    struct ClosureKernelCmp {
        bool operator()(LALRClosure const &m1, LALRClosure const &m2) const {
            auto i1 = m1.begin(), i2 = m2.begin(), e1 = m1.end(), e2 = m2.end();
            for (; (i1 != e1) && (i2 != e2); (void)++i1, (void)++i2) {
                if (i1->first != i2->first)
                    return i1->first < i2->first;
            }
            return i1 == e1 && i2 != e2;
        };
    };

  public:
    explicit LALRParser(Grammar const &g) : LRParser(g) {}

    // We have buildNFA() in base class build a LR0 NFA automaton for us.
    void buildDFA() override {
        Automaton &M = dfa;

        // const auto &symbols = gram.getAllSymbols();
        // auto const &productionTable = gram.getProductionTable();
        auto const &lr0States = nfa.getAllStates();
        auto const epsilonID = gram.getEpsilonSymbol().id;

        M.actions = nfa.actions;
        M.transformedDFAFlag = true;
        M.setDumpFlag(true);

        // <LALRClosure, ClosureID>
        std::map<LALRClosure, int, ClosureKernelCmp> closureIndexMap;
        // Stores iterators of unvisited closureIndexMap.
        // Stack should be the same as queue. but queue is easier to debug.
        std::queue<decltype(closureIndexMap.cbegin())> queue;

        // Prepare the first closure
        auto const &startState = lr0States.front();
        LALRClosure startClosure;
        startClosure.emplace(startState.id, *startState.constraint);
        makeClosure(lr0States, startClosure);
        // Add the first closure
        M.addPseudoState();
        M.markStartState(StateID{0});
        queue.push(closureIndexMap.emplace(std::move(startClosure), 0).first);

        while (!queue.empty()) {
            auto closureIter = queue.front();
            queue.pop();

            // Try different actions
            auto nAction = static_cast<int>(M.actions.size());
            for (int i = 0; i < nAction; ++i) {
                if (i == epsilonID)
                    continue;
                auto actionID = static_cast<ActionID>(i);
                auto newClosure =
                    transit(lr0States, actionID, closureIter->first);

                // Cannot accept this action
                if (newClosure.empty())
                    continue;

                auto iter = closureIndexMap.find(newClosure);
                if (iter == closureIndexMap.end()) {
                    // Add new closure
                    auto closureID =
                        static_cast<StateID>(closureIndexMap.size());
                    queue.push(closureIndexMap
                                   .emplace(std::move(newClosure), closureID)
                                   .first);
                    M.addPseudoState();
                    // Add link to this closure
                    M.addTransition(StateID{closureIter->second}, closureID,
                                    actionID);
                } else {
                    // Merge closures.
                    // Now number of elements in two maps should be the same.
                    bool effectFlag = false;
                    auto i1 = iter->first.begin(), e1 = iter->first.end();
                    auto i2 = newClosure.begin();
                    for (; i1 != e1; (void)++i1, (void)++i2) {
                        auto before = i1->second;
                        *const_cast<Constraint *>(&i1->second) |= i2->second;
                        if (i1->second != before)
                            effectFlag = true;
                    }
                    M.addTransition(StateID{closureIter->second},
                                    StateID{iter->second}, actionID);
                    // The merged state should be checked again
                    if (effectFlag)
                        queue.push(iter);
                }
            }
        }

        // Now we have all closureIndexMap, we have to put them into automaton.
        auto hashFunc = [](const Constraint *arg) {
            return std::hash<Constraint>()(*arg);
        };
        auto equalFunc = [](const Constraint *arg1, const Constraint *arg2) {
            return *arg1 == *arg2;
        };
        auto estimatedConstraintCount = nfa.states.size();
        std::unordered_set<Constraint *, decltype(hashFunc),
                           decltype(equalFunc)>
            constraintSet(estimatedConstraintCount, hashFunc, equalFunc);
        // If constraint is new, it will be moved.
        auto storeConstraint = [&constraintSet, this](Constraint *constraint) {
            auto it = constraintSet.find(constraint);
            if (it != constraintSet.end())
                return *it;
            auto res = newConstraint(std::move(*constraint));
            constraintSet.insert(res);
            return res;
        };
        // 1. Add aux states
        // 2. Build bitset
        auto lr0AuxEnd = this->auxEnd;
        std::vector<State> &auxStates = M.auxStates; // Size is unknown yet
        M.closures.resize(closureIndexMap.size());   // Size is known
        assert(auxStates.empty());
        for (auto &[lalrClosure, closureIndex] : closureIndexMap) {
            // BitSet that will be moved into automaton.
            util::BitSet<StateID> closure;
            for (auto &[lr0State, constraint] : lalrClosure) {
                auto auxIndex = static_cast<StateID>(auxStates.size());
                // Copy original state and change its ID and constraint
                State auxState = lr0States[lr0State];
                auxState.id = auxIndex;
                auxState.constraint =
                    storeConstraint(const_cast<Constraint *>(&constraint));
                auxStates.push_back(auxState);
                closure.insert(auxIndex);
                if (lr0State == lr0AuxEnd) {
                    assert(this->auxEnd == lr0AuxEnd ||
                           this->auxEnd == auxIndex);
                    this->auxEnd = StateID{auxIndex};
                }
            }
            M.closures[closureIndex] = std::move(closure);
        }

        display(AUTOMATON, INFO, "DFA is built", &dfa, (void *)"build_dfa");
    }

  protected:
    [[nodiscard]] bool
    canAddParseTableEntry(StateID state, ActionID act, ParseAction pact,
                          StateID auxStateID) const override {
        // We need to check specific reduction rule
        if (pact.type == ParseAction::REDUCE) {
            auto const &auxStates = dfa.getAuxStates();
            auto const &auxState = auxStates[auxStateID];
            return auxState.constraint->contains(act);
        }

        return true;
    }

    // Do not mark seed
    [[nodiscard]] bool
    canMarkFinal(const StateSeed &seed,
                 Production const &production) const override {
        // auto endActionID =
        // static_cast<ActionID>(gram.getEndOfInputSymbol().id);
        // assert(seed.second);
        // return seed.second->contains(endActionID);
        return false;
    }

    // Only build a LR0 automaton
    [[nodiscard]] bool shouldIncludeConstraintsInDump() const override {
        return false;
    }

    // Although LALRParser does have constraints, we cannot put the algorithm
    // here because we want buildNFA() process to build a pure LR0 NFA automaton
    // for us.
    [[nodiscard]] util::BitSet<ActionID> *
    resolveLocalConstraints(const util::BitSet<ActionID> *parentConstraint,
                            const Production &production,
                            int rhsIndex) override {
        return allTermConstraint;
    }
};
} // namespace gram

#endif