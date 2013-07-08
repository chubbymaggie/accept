#include "accept.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/PostDominators.h"

#include <sstream>

using namespace llvm;

const char *FUNC_BARRIER = "pthread_barrier_wait";
const char *FUNC_PARSEC_BARRIER = "_Z19parsec_barrier_waitP16parsec_barrier_t";
bool isBarrier(Instruction *inst) {
  return isCallOf(inst, FUNC_BARRIER) || isCallOf(inst, FUNC_PARSEC_BARRIER);
}

void instructionsBetweenHelper(Instruction *end,
    std::set<Instruction *> &instrs, BasicBlock *curBB,
    std::set<BasicBlock *> &visited) {
  // Mark visit.
  if (visited.count(curBB))
    return;
  visited.insert(curBB);

  // Collect instructions.
  for (BasicBlock::iterator i = curBB->begin(); i != curBB->end(); ++i) {
    if (end == i) {
      return;
    } else if (curBB->getTerminator() != i) {
      instrs.insert(i);
    }
  }

  // Recurse into successors.
  TerminatorInst *term = curBB->getTerminator();
  if (term->getNumSuccessors() == 0) {
    errs() << "found exit in begin/end chain!\n";
    return;
  }
  for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
    instructionsBetweenHelper(end, instrs, term->getSuccessor(i), visited);
  }
}

void instructionsBetween(Instruction *start, Instruction *end,
                         std::set<Instruction *> &instrs) {
  // Handle the first basic block.
  BasicBlock *startBB = start->getParent();
  bool entered;
  for (BasicBlock::iterator i = startBB->begin(); i != startBB->end(); ++i) {
    if (!entered && start == i) {
      entered = true;
    } else if (entered && end == i) {
      return;
    } else if (entered && start != i) {
      if (startBB->getTerminator() != i)
        instrs.insert(i);
    }
  }

  // Recurse into successors.
  std::set<BasicBlock *> visited;
  visited.insert(startBB);
  TerminatorInst *term = startBB->getTerminator();
  if (term->getNumSuccessors() == 0) {
    errs() << "found exit in begin/end chain!\n";
    return;
  }
  for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
    instructionsBetweenHelper(end, instrs, term->getSuccessor(i), visited);
  }
}

// Given an acquire call or a barrier call, find all the instructions between
// it and a corresponding release call or the next barrier. The instructions
// in the critical section are collected into the set supplied. Returns the
// release/next barrier instruction if one is found or NULL otherwise.
Instruction *ACCEPTPass::findCritSec(Instruction *acq,
                                     std::set<Instruction*> &cs) {
  bool acquired = false;
  BasicBlock *bb = acq->getParent();

  bool isLock;
  if (isAcquire(acq)) {
    isLock = true;
  } else if (isBarrier(acq)) {
    isLock = false;
  } else {
    errs() << "not a critical section entry!\n";
    return NULL;
  }

  // Look for a release call that is dominated by the acquire and
  // post-dominates the acquire.
  DominatorTree &domTree = getAnalysis<DominatorTree>();
  PostDominatorTree &postDomTree = getAnalysis<PostDominatorTree>();
  Function *func = acq->getParent()->getParent();
  Instruction *rel = NULL;
  for (Function::iterator fi = func->begin(); fi != func->end(); ++fi) {
    for (BasicBlock::iterator bi = fi->begin(); bi != fi->end(); ++bi) {
      if ((isLock && isRelease(bi)) ||
          (!isLock && acq != bi && isBarrier(bi))) {
        // Candidate pair.
        if (domTree.dominates(acq, bi) &&
            postDomTree.dominates(bi->getParent(), acq->getParent())) {
          rel = bi;
          break;
        }
      }
    }
  }

  if (rel == NULL) {
    *log << "no matching sync found\n";
    return NULL;
  }

  instructionsBetween(acq, rel, cs);
  for (std::set<Instruction *>::iterator i = cs.begin();
        i != cs.end(); ++i) {
    if (isAcquire(*i) || isRelease(*i) || isBarrier(*i)) {
      *log << "nested sync\n";
      return NULL;
    }
  }

  return rel;
}


std::string ACCEPTPass::siteName(std::string kind, Instruction *at) {
  std::stringstream ss;
  ss << kind << " at "
     << srcPosDesc(*module, at->getDebugLoc());
  return ss.str();
}


// Find the critical section beginning with an acquire (or barrier), check for
// approximateness, and return the release (or next barrier). If the critical
// section cannot be identified or is not approximate, return null.
Instruction *ACCEPTPass::findApproxCritSec(Instruction *acq) {
  // Find all the instructions between this acquire and the next release.
  std::set<Instruction*> critSec;
  Instruction *rel = findCritSec(acq, critSec);
  if (!rel) {
    return NULL;
  }

  // Check for precise side effects.
  std::set<Instruction*> blessed;
  blessed.insert(rel);
  critSec.insert(rel);
  std::set<Instruction*> blockers = AI->preciseEscapeCheck(critSec, &blessed);
  *log << "blockers: " << blockers.size() << "\n";
  for (std::set<Instruction*>::iterator i = blockers.begin();
        i != blockers.end(); ++i) {
    *log << " * " << instDesc(*module, *i) << "\n";
  }
  if (blockers.size()) {
    return NULL;
  }

  return rel;
}


bool ACCEPTPass::optimizeAcquire(Instruction *acq, int id) {
  // Generate a name for this opportunity site.
  std::string optName = siteName("lock acquire", acq);
  *log << "---\n" << optName << "\n";

  Instruction *rel = findApproxCritSec(acq);
  if (!rel)
    return false;

  // Success.
  *log << "can elide lock " << id << "\n";
  if (relax) {
    int param = relaxConfig[id];
    if (param) {
      // Remove the acquire and release calls.
      *log << "eliding lock\n";
      acq->eraseFromParent();
      rel->eraseFromParent();
      return true;
    }
  } else {
    relaxConfig[id] = 0;
    configDesc[id] = optName;
  }
  return false;
}



bool ACCEPTPass::optimizeBarrier(Instruction *bar1, int id) {
  std::string optName = siteName("barrier", bar1);
  *log << "---\n" << optName << "\n";

  if (!findApproxCritSec(bar1))
    return false;

  // Success.
  *log << "can elide barrier " << id << "\n";
  if (relax) {
    int param = relaxConfig[id];
    if (param) {
      // Remove the first barrier.
      *log << "eliding barrier wait\n";
      bar1->eraseFromParent();
      return true;
    }
  } else {
    relaxConfig[id] = 0;
    configDesc[id] = optName;
  }
  return false;
}


bool ACCEPTPass::optimizeSync(Function &F) {
  bool changed = false;
  for (Function::iterator fi = F.begin(); fi != F.end(); ++fi) {
    for (BasicBlock::iterator bi = fi->begin(); bi != fi->end(); ++bi) {
      if (isAcquire(bi) || isBarrier(bi)) {
        bool optimized;
        if (isAcquire(bi))
          optimized = optimizeAcquire(bi, opportunityId);
        else
          optimized = optimizeBarrier(bi, opportunityId);
        changed |= optimized;
        ++opportunityId;
        if (optimized)
          // Stop iterating over this block, since it changed (and there's
          // almost certainly not another critical section in here anyway).
          break;
      }
    }
  }
  return changed;
}
