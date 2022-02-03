#ifndef LRPARSER_LR1_H
#define LRPARSER_LR1_H

#include "src/automata/Automaton.h"
#include "src/common.h"
#include "src/grammar/Grammar.h"
#include "src/parser/LRParser.h"
#include "src/util/BitSet.h"

namespace gram {
class LR1Parser : public LRParser {
  public:
    explicit LR1Parser(Grammar const &g) : LRParser(g) {}

    [[nodiscard]] bool canAddParseTableEntry(StateID state, ActionID act,
                                             ParseAction pact,
                                             StateID substate) const override {
        // We need to check specific reduction rule
        if (pact.type == ParseAction::REDUCE) {
            auto const &auxStates = dfa.getAuxStates();
            auto const &auxState = auxStates[substate];
            return auxState.constraint->contains(act);
        }

        return true;
    }

    [[nodiscard]] bool
    canMarkFinal(const StateSeed &seed,
                 Production const &production) const override {
        auto endActionID = static_cast<ActionID>(gram.getEndOfInputSymbol().id);
        assert(seed.second);
        return seed.second->contains(endActionID);
    }

    [[nodiscard]] bool shouldIncludeConstraintsInDump() const override {
        return true;
    }

    [[nodiscard]] std::function<size_t(const StateSeed &)>
    getSeedHashFunc() const override {
        return [](StateSeed const &seed) -> std::size_t {
            size_t res = std::hash<int>()(seed.first);
            res = res * 31 + std::hash<util::BitSet<ActionID>>()(*seed.second);
            return res;
        };
    }

    [[nodiscard]] std::function<bool(const StateSeed &, const StateSeed &)>
    getSeedEqualFunc() const override {
        return [](StateSeed const &s1, StateSeed const &s2) -> bool {
            return s1.first == s2.first && *s1.second == *s2.second;
        };
    }

    [[nodiscard]] util::BitSet<ActionID> *
    resolveLocalConstraints(const util::BitSet<ActionID> *parentConstraint,
                            const Production &production,
                            int rhsIndex) override {
        auto const &symbols = gram.getAllSymbols();
        auto const &rhs = production.rightSymbols;
        util::BitSet<ActionID> constraints(symbols.size());

        bool allNullable = true;
        for (size_t i = rhsIndex + 1; allNullable && i < rhs.size(); ++i) {
            if (!symbols[rhs[i]].nullable.value())
                allNullable = false;
            constraints |= symbols[rhs[i]].firstSet;
        }
        if (allNullable && parentConstraint)
            constraints |= *parentConstraint;

        return newConstraint(std::move(constraints));
    }
};
} // namespace gram

#endif