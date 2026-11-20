// UndoHistory.h — undo / redo of whole-state snapshots, GUI-free (DSP Studio keeps its DspParams here).
//
// The window calls Commit() once an editing gesture has settled (a drag that moved a fader a hundred
// times is ONE step). The baseline is the state at the previous commit; Commit() stores it as the thing
// Undo() returns to. Changes that must not be undoable (the file was edited by another program) go
// through Rebase().
#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace vac3dsp {

template <class T>
class UndoHistory
{
public:
  using Same = std::function<bool(const T&, const T&)>;

  explicit UndoHistory(Same same, size_t limit = 200) : same_(std::move(same)), limit_(limit) {}

  // New starting point; the history is forgotten.
  void Reset(const T& current)
  {
    undo_.clear();
    redo_.clear();
    baseline_ = current;
  }

  // The state changed by something that is not an edit of the user: follow it without recording a step.
  void Rebase(const T& current) { baseline_ = current; }

  // The user edited the state. Returns true when this created an undo step (an unchanged state does not).
  bool Commit(const T& current)
  {
    if (same_(current, baseline_))
      return false;
    undo_.push_back(baseline_);
    if (undo_.size() > limit_)
      undo_.erase(undo_.begin());
    redo_.clear();
    baseline_ = current;
    return true;
  }

  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }

  // `current` must have been committed first. On success it becomes the previous state.
  bool Undo(T& current)
  {
    if (undo_.empty())
      return false;
    redo_.push_back(current);
    current = undo_.back();
    undo_.pop_back();
    baseline_ = current;
    return true;
  }

  bool Redo(T& current)
  {
    if (redo_.empty())
      return false;
    undo_.push_back(current);
    current = redo_.back();
    redo_.pop_back();
    baseline_ = current;
    return true;
  }

  size_t UndoDepth() const { return undo_.size(); }
  size_t RedoDepth() const { return redo_.size(); }

private:
  Same same_;
  size_t limit_;
  std::vector<T> undo_, redo_;
  T baseline_{};
};

}  // namespace vac3dsp
