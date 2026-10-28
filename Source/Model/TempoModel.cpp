#include "TempoModel.h"

namespace atomic
{

TempoModel::TempoModel()
{
    resetToDefault (timeline::defaultLengthBeats);
}

void TempoModel::resetToDefault (double lengthBeats)
{
    // 0.5 is 120 BPM through the lane's own mapping (60 * 4^0.5). The lane's
    // constructor default of 1.0 would mean 240 BPM, so "新工程打开了速度自动化"
    // would double the project's speed before the user drew anything.
    lane.resetFlat (tempo::valueForBpm (tempo::defaultBpm), lengthBeats);
    lane.setEnabled (false);
}

void TempoModel::setAutomated (bool shouldAutomate)
{
    if (lane.isEnabled() == shouldAutomate)
        return;

    lane.setEnabled (shouldAutomate);
    notify();
}

TempoMap TempoModel::buildMap (double manualBpm, double lengthBeats) const
{
    return TempoMap::fromLane (lane, manualBpm, lengthBeats);
}

void TempoModel::notify()
{
    listeners.call ([] (Listener& l) { l.tempoChanged(); });
}

} // namespace atomic
