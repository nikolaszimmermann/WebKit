/*
 * Copyright (C) 2015-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DFGPhantomInsertionPhase.h"

#if ENABLE(DFG_JIT)

#include "DFGForAllKills.h"
#include "DFGGraph.h"
#include "DFGInsertionSet.h"
#include "DFGMayExit.h"
#include "DFGPhase.h"
#include "JSCJSValueInlines.h"
#include "OperandsInlines.h"

namespace JSC { namespace DFG {

namespace {

class PhantomInsertionPhase : public Phase {
    static constexpr bool verbose = false;
public:
    PhantomInsertionPhase(Graph& graph)
        : Phase(graph, "phantom insertion"_s)
        , m_insertionSet(graph)
        , m_values(OperandsLike, graph.block(0)->variablesAtHead)
    {
    }
    
    bool run()
    {
        // We assume that DCE has already run. If we run before DCE then we think that all
        // SetLocals execute, which is inaccurate. That causes us to insert too few Phantoms.
        DFG_ASSERT(m_graph, nullptr, m_graph.m_refCountState == ExactRefCount);
        
        dataLogIf(verbose, "Graph before Phantom insertion:\n", m_graph);
        
        m_graph.clearEpochs();
        
        for (BasicBlock* block : m_graph.blocksInNaturalOrder())
            handleBlock(block);
        
        dataLogIf(verbose, "Graph after Phantom insertion:\n", m_graph);
        
        return true;
    }

private:
    void handleBlock(BasicBlock* block)
    {
        // FIXME: For blocks that have low register pressure, it would make the most sense to
        // simply insert Phantoms at the last point possible since that would obviate the need to
        // query bytecode liveness:
        //
        // - If we MovHint @x into loc42 then put a Phantom on the last MovHinted value in loc42.
        // - At the end of the block put Phantoms for each MovHinted value.
        //
        // This will definitely not work if there are any phantom allocations. For those blocks
        // where this would be legal, it remains to be seen how profitable it would be even if there
        // was high register pressure. After all, a Phantom would cause a spill but it wouldn't
        // cause a fill.
        //
        // https://bugs.webkit.org/show_bug.cgi?id=144524
        
        m_values.fill(nullptr);

        Epoch currentEpoch = Epoch::first();
        unsigned lastExitingIndex = 0;
        for (unsigned nodeIndex = 0; nodeIndex < block->size(); ++nodeIndex) {
            Node* node = block->at(nodeIndex);
            dataLogLnIf(verbose, "Considering ", node);
            
            switch (node->op()) {
            case MovHint:
            case ZombieHint:
                m_values.operand(node->unlinkedOperand()) = node->child1().node();
                break;

            case GetLocal:
            case SetArgumentDefinitely:
            case SetArgumentMaybe:
                m_values.operand(node->operand()) = nullptr;
                break;
                
            default:
                break;
            }

            bool nodeMayExit = mayExit(m_graph, node) != DoesNotExit;
            if (nodeMayExit) {
                currentEpoch.bump();
                lastExitingIndex = nodeIndex;
            }
            
            m_graph.doToChildren(
                node,
                [&] (Edge edge) {
                    dataLogLnIf(verbose, "Updating epoch for ", edge, " to ", currentEpoch);
                    edge->setEpoch(currentEpoch);
                });
            
            node->setEpoch(currentEpoch);

            Operand alreadyKilled;

            auto processKilledOperand = [&] (Operand operand) {
                dataLogLnIf(verbose, "    Killed operand: ", operand);

                // Already handled from SetLocal.
                if (operand == alreadyKilled) {
                    dataLogLnIf(verbose, "    Operand ", operand, " already killed by set local");
                    return;
                }
                
                Node* killedNode = m_values.operand(operand); 
                if (!killedNode) {
                    dataLogLnIf(verbose, "    Operand ", operand, " was not defined in this block.");
                    return;
                }

                m_values.operand(operand) = nullptr;
                
                // We only need to insert a Phantom if the node hasn't been used since the last
                // exit, and was born before the last exit.
                if (killedNode->epoch() == currentEpoch) {
                    dataLogLnIf(verbose, "    Operand ", operand, " has current epoch ", currentEpoch);
                    return;
                }
                
                dataLogLnIf(verbose,
                    "    Inserting Phantom on ", killedNode, " after ",
                    block->at(lastExitingIndex));
                
                // We have exact ref counts, so creating a new use means that we have to
                // increment the ref count.
                killedNode->postfixRef();

                Node* lastExitingNode = block->at(lastExitingIndex);
                
                m_insertionSet.insertNode(
                    lastExitingIndex + 1, SpecNone, Phantom,
                    lastExitingNode->origin.forInsertingAfter(m_graph, lastExitingNode),
                    killedNode->defaultEdge());
            };

            if (node->op() == SetLocal) {
                Operand operand = node->operand();
                if (nodeMayExit) {
                    // If the SetLocal does exit, we need the MovHint of its local
                    // to be live until the SetLocal is done.
                    processKilledOperand(operand);
                    alreadyKilled = operand;
                }
                m_values.operand(operand) = nullptr;
            }

            forAllKilledOperands(m_graph, node, block->tryAt(nodeIndex + 1), processKilledOperand);
        }
        
        m_insertionSet.execute(block);
    }
    
    InsertionSet m_insertionSet;
    Operands<Node*> m_values;
};

} // anonymous namespace
    
bool performPhantomInsertion(Graph& graph)
{
    return runPhase<PhantomInsertionPhase>(graph);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)

