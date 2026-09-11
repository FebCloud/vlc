/*
 * PlaylistManager.cpp
 *****************************************************************************
 * Copyright © 2010 - 2011 Klagenfurt University
 *             2015 VideoLAN and VLC Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "PlaylistManager.h"
#include "SegmentTracker.hpp"
#include "SharedResources.hpp"
#include "playlist/BasePlaylist.hpp"
#include "playlist/BasePeriod.h"
#include "playlist/BaseAdaptationSet.h"
#include "playlist/BaseRepresentation.h"
#include "http/HTTPConnectionManager.h"
#include "logic/AlwaysBestAdaptationLogic.h"
#include "logic/RateBasedAdaptationLogic.h"
#include "logic/AlwaysLowestAdaptationLogic.hpp"
#include "logic/PredictiveAdaptationLogic.hpp"
#include "logic/NearOptimalAdaptationLogic.hpp"
#include "logic/BufferingLogic.hpp"
#include "tools/Debug.hpp"
#ifdef ADAPTIVE_DEBUGGING_LOGIC
# include "logic/RoundRobinLogic.hpp"
#endif
#include <vlc_stream.h>
#include <vlc_demux.h>
#include <vlc_threads.h>

#include <algorithm>
#include <cassert>
#include <ctime>
#include <limits>
#include <set>

using namespace adaptive::http;
using namespace adaptive::logic;
using namespace adaptive;
using vlc::threads::mutex_locker;

PlaylistManager::PlaylistManager( demux_t *p_demux_,
                                  SharedResources *res,
                                  BasePlaylist *pl,
                                  AbstractStreamFactory *factory,
                                  AbstractAdaptationLogic::LogicType type ) :
             logicType      ( type ),
             logic          ( nullptr ),
             playlist       ( pl ),
             streamFactory  ( factory ),
             p_demux        ( p_demux_ ),
             startupPolicy  ( var_InheritBool(p_demux_, "adaptive-fast-start"),
                              VLC_TICK_FROM_MS(var_InheritInteger(p_demux_,
                                                                 "adaptive-startup-buffer")),
                              var_InheritBool(p_demux_, "adaptive-startup-parallel"),
                              var_InheritBool(p_demux_, "adaptive-startup-media") )
{
    currentPeriod = playlist->getFirstPeriod();
    resources = res;
    bufferingLogic = nullptr;
    failedupdates = 0;
    b_thread = false;
    b_buffering = false;
    b_canceled = false;
    startupPreparationPending = startupPolicy.shouldPrepareInParallel();
    b_preparsing = false;
    nextPlaylistupdate = 0;
    demux.pcr_syncpoint = TimestampSynchronizationPoint::RandomAccess;
    vlc_mutex_init(&demux.lock);
    vlc_cond_init(&demux.cond);
    vlc_mutex_init(&cached.lock);
    vlc_mutex_init(&adaptiveControl.lock);
    adaptiveControl.pendingBoundarySet = nullptr;
    adaptiveControl.pendingRepresentationID.clear();
    cached.b_live = false;
    cached.f_position = 0.0;
    cached.i_time = VLC_TICK_INVALID;
    cached.i_normaltime = VLC_TICK_INVALID;
    cached.playlistStart = 0;
    cached.playlistEnd = 0;
    cached.playlistLength = 0;
    cached.lastupdate = 0;
    if(startupPreparationPending)
        resources->getConnManager()->setStartupConcurrency(true);
}

PlaylistManager::~PlaylistManager   ()
{
    delete streamFactory;
    unsetPeriod();
    delete playlist;
    delete resources;
    delete logic;
    delete bufferingLogic;
}

void PlaylistManager::unsetPeriod()
{
    {
        vlc_mutex_locker locker(&adaptiveControl.lock);
        if(logic)
            logic->setManualRepresentation(nullptr, "");
        adaptiveControl.variants.clear();
        adaptiveControl.currentRepresentationID.clear();
        adaptiveControl.pendingBoundarySet = nullptr;
        adaptiveControl.pendingRepresentationID.clear();
    }

    std::vector<AbstractStream *>::iterator it;
    for(it=streams.begin(); it!=streams.end(); ++it)
        delete *it;
    streams.clear();
}

bool PlaylistManager::setupPeriod()
{
    if(!currentPeriod)
        return false;

    if(!logic && !(logic = createLogic(logicType, resources->getConnManager())))
        return false;

    if(!bufferingLogic && !(bufferingLogic = createBufferingLogic()))
        return false;

    const std::vector<BaseAdaptationSet*> &sets = currentPeriod->getAdaptationSets();
    for(BaseAdaptationSet *set : sets)
    {
        if(set && streamFactory)
        {
            SegmentTracker *tracker = new SegmentTracker(resources, logic,
                                                         bufferingLogic, set,
                                                         &synchronizationReferences);
            if(!tracker)
                continue;

            AbstractStream *st = streamFactory->create(p_demux, set->getStreamFormat(),
                                                       tracker);
            if(!st)
            {
                delete tracker;
                continue;
            }

            tracker->registerListener(this);

            streams.push_back(st);

            /* Generate stream description */
            if(!set->getLang().empty())
                st->setLanguage(set->getLang());

            if(!set->description.empty())
                st->setDescription(set->description);
        }
    }
    refreshAdaptiveVariants();
    return true;
}

enum class RepresentationKind
{
    Video,
    NonVideo,
    Unknown,
};

static RepresentationKind representationKind(BaseRepresentation *rep)
{
    if(rep->getWidth() > 0 || rep->getHeight() > 0)
        return RepresentationKind::Video;

    bool hasNonVideoCodec = false;
    CodecDescriptionList descriptions;
    rep->getCodecsDesc(&descriptions);
    for(const CodecDescription *description : descriptions)
    {
        const es_format_t *format = description->getFmt();
        if(format && format->i_cat == VIDEO_ES)
            return RepresentationKind::Video;
        if(format && (format->i_cat == AUDIO_ES || format->i_cat == SPU_ES))
            hasNonVideoCodec = true;
    }
    return hasNonVideoCodec ? RepresentationKind::NonVideo
                            : RepresentationKind::Unknown;
}

void PlaylistManager::refreshAdaptiveVariants()
{
    std::vector<AdaptiveVariantInfo> variants;
    std::set<std::string> ids;

    if(currentPeriod)
    {
        for(BaseAdaptationSet *set : currentPeriod->getAdaptationSets())
        {
            if(!set)
                continue;
            for(BaseRepresentation *rep : set->getRepresentations())
            {
                if(!rep || representationKind(rep) != RepresentationKind::Video)
                    continue;

                const std::string id = rep->getID().str();
                if(id.empty() || !ids.insert(id).second)
                    continue;

                std::string codecs;
                for(const std::string &codec : rep->getCodecs())
                {
                    if(!codecs.empty())
                        codecs.append(",");
                    codecs.append(codec);
                }

                const Rate &rate = rep->getFrameRate();
                AdaptiveVariantInfo info = {
                    id,
                    set,
                    rep->getWidth() > 0 ? static_cast<unsigned>(rep->getWidth()) : 0,
                    rep->getHeight() > 0 ? static_cast<unsigned>(rep->getHeight()) : 0,
                    rep->getBandwidth(),
                    rate.num(),
                    rate.den(),
                    codecs,
                };
                variants.push_back(info);
            }
        }
    }

    std::sort(variants.begin(), variants.end(),
              [](const AdaptiveVariantInfo &a, const AdaptiveVariantInfo &b) {
                  if(a.height != b.height)
                      return a.height < b.height;
                  if(a.width != b.width)
                      return a.width < b.width;
                  return a.bandwidth < b.bandwidth;
              });

    vlc_mutex_locker locker(&adaptiveControl.lock);
    adaptiveControl.variants.swap(variants);
    adaptiveControl.pendingBoundarySet = nullptr;
    adaptiveControl.pendingRepresentationID.clear();
}

int PlaylistManager::getAdaptiveVariants(vlc_adaptive_variant_t **pp_variants,
                                         size_t *p_count) const
{
    if(!pp_variants || !p_count)
        return VLC_EGENERIC;
    *pp_variants = nullptr;
    *p_count = 0;

    vlc_mutex_locker locker(&adaptiveControl.lock);
    if(adaptiveControl.variants.empty())
        return VLC_SUCCESS;

    vlc_adaptive_variant_t *out = static_cast<vlc_adaptive_variant_t *>(
        calloc(adaptiveControl.variants.size(), sizeof(*out)));
    if(!out)
        return VLC_ENOMEM;

    size_t copied = 0;
    for(const AdaptiveVariantInfo &source : adaptiveControl.variants)
    {
        vlc_adaptive_variant_t &dest = out[copied];
        dest.psz_id = strdup(source.id.c_str());
        dest.psz_codecs = strdup(source.codecs.c_str());
        if(!dest.psz_id || !dest.psz_codecs)
        {
            free(dest.psz_id);
            free(dest.psz_codecs);
            for(size_t i = 0; i < copied; ++i)
            {
                free(out[i].psz_id);
                free(out[i].psz_codecs);
            }
            free(out);
            return VLC_ENOMEM;
        }
        dest.width = source.width;
        dest.height = source.height;
        dest.bandwidth = source.bandwidth;
        dest.frame_rate_num = source.frameRateNum;
        dest.frame_rate_den = source.frameRateDen;
        ++copied;
    }

    *pp_variants = out;
    *p_count = copied;
    return VLC_SUCCESS;
}

int PlaylistManager::setAdaptiveVariant(const char *psz_id)
{
    if(!logic)
        return VLC_EGENERIC;

    const std::string id = psz_id ? psz_id : "";
    BaseAdaptationSet *set = nullptr;
    {
        vlc_mutex_locker locker(&adaptiveControl.lock);
        if(!id.empty())
        {
            const auto found = std::find_if(adaptiveControl.variants.cbegin(),
                                            adaptiveControl.variants.cend(),
                [&id](const AdaptiveVariantInfo &variant) { return variant.id == id; });
            if(found == adaptiveControl.variants.cend())
                return VLC_EGENERIC;
            set = found->adaptationSet;
        }
    }

    {
        vlc_mutex_locker locker(&adaptiveControl.lock);
        if(!set)
        {
            logic->setManualRepresentation(nullptr, "");
            adaptiveControl.pendingBoundarySet = nullptr;
            adaptiveControl.pendingRepresentationID.clear();
        }
        else if(!started())
        {
            logic->setManualRepresentation(set, id);
            adaptiveControl.pendingBoundarySet = nullptr;
            adaptiveControl.pendingRepresentationID.clear();
        }
        else
        {
            /* Keep the current bufferize() on the old representation. Applying
             * the pin immediately lets prepareChunk() switch mid-download, after
             * which boundary prepare sees target==current and becomes a no-op. */
            adaptiveControl.pendingBoundarySet = set;
            adaptiveControl.pendingRepresentationID = id;
        }
    }
    if(set && started())
        waitcond.signal();

    msg_Info(p_demux, "adaptive quality selection changed to %s%s",
             id.empty() ? "auto" : id.c_str(),
             set && started() ? "; boundary preparation queued" : "");
    return VLC_SUCCESS;
}

void PlaylistManager::preparePendingAdaptiveSwitch()
{
    BaseAdaptationSet *set;
    std::string id;
    {
        vlc_mutex_locker locker(&adaptiveControl.lock);
        set = adaptiveControl.pendingBoundarySet;
        id = adaptiveControl.pendingRepresentationID;
        adaptiveControl.pendingBoundarySet = nullptr;
        adaptiveControl.pendingRepresentationID.clear();
        if(set)
            logic->setManualRepresentation(set, id);
    }
    if(!set)
        return;

    Times playback;
    {
        vlc_mutex_locker locker(&demux.lock);
        playback = demux.times;
    }

    bool boundarySwitch = false;
    for(AbstractStream *stream : streams)
        boundarySwitch |= stream->prepareSwitchAtNextSegment(set, playback);

    if(!boundarySwitch)
        msg_Warn(p_demux,
                 "adaptive manual switch boundary prepare failed; "
                 "falling back to delayed representation switch");
}

int PlaylistManager::getAdaptiveVariant(char **ppsz_id) const
{
    if(!ppsz_id)
        return VLC_EGENERIC;
    vlc_mutex_locker locker(&adaptiveControl.lock);
    *ppsz_id = strdup(adaptiveControl.currentRepresentationID.c_str());
    return *ppsz_id ? VLC_SUCCESS : VLC_ENOMEM;
}

void PlaylistManager::trackerEvent(const TrackerEvent &event)
{
    if(event.getType() != TrackerEvent::Type::RepresentationSwitch)
        return;

    const RepresentationSwitchEvent &switched =
        static_cast<const RepresentationSwitchEvent &>(event);
    const bool previousWasVideo = switched.prev &&
        representationKind(switched.prev) == RepresentationKind::Video;
    const bool nextIsVideo = switched.next &&
        representationKind(switched.next) == RepresentationKind::Video;
    if(!previousWasVideo && !nextIsVideo)
        return;

    vlc_mutex_locker locker(&adaptiveControl.lock);
    adaptiveControl.currentRepresentationID = nextIsVideo
        ? switched.next->getID().str() : std::string();
}

bool PlaylistManager::init(bool b_preparsing)
{
    this->b_preparsing = b_preparsing;

    if(!setupPeriod())
        return false;

    playlist->playbackStart = time(nullptr);
    nextPlaylistupdate = playlist->playbackStart;

    if(b_preparsing)
        preparsePlaylist();
    updateControlsPosition();

    return true;
}

bool PlaylistManager::start()
{
    if(b_thread || b_preparsing)
        return false;

    b_thread = !vlc_clone(&thread, managerThread, static_cast<void *>(this));
    if(!b_thread)
        return false;

    setBufferingRunState(true);

    return true;
}

bool PlaylistManager::started() const
{
    return b_thread;
}

void PlaylistManager::stop()
{
    if(!b_thread)
        return;

    {
        mutex_locker locker {lock};
        b_canceled = true;
        waitcond.signal();
    }

    vlc_join(thread, nullptr);
    b_thread = false;
}

struct PrioritizedAbstractStream
{
    AbstractStream::BufferingStatus status;
    vlc_tick_t demuxed_amount;
    AbstractStream *st;
};

static bool streamCompare(const PrioritizedAbstractStream &a,  const PrioritizedAbstractStream &b)
{
    if( a.status >= b.status ) /* Highest prio is higher value in enum */
    {
        if ( a.status == b.status ) /* Highest prio is lowest buffering */
           return a.demuxed_amount < b.demuxed_amount;
        else
            return true;
    }
    return false;
}

struct StartupPreparationTask
{
    AbstractStream *stream;
    bool throughMedia;
    bool succeeded;
};

static void * prepareStartupStream(void *opaque)
{
    StartupPreparationTask *task = static_cast<StartupPreparationTask *>(opaque);
    task->succeeded = task->stream->prepareStartupChunk(task->throughMedia);
    return nullptr;
}

void PlaylistManager::prepareStartupStreams()
{
    if(!startupPreparationPending)
        return;
    startupPreparationPending = false;

    std::vector<StartupStreamCandidate> candidates;
    candidates.reserve(streams.size());
    for(AbstractStream *stream : streams)
    {
        StartupStreamCandidate candidate = {
            stream->startupStreamKind(),
            stream->isValid(),
            stream->esCount() > 0 && stream->isSelected(),
            stream->isDisabled(),
        };
        candidates.push_back(candidate);
    }

    const StartupStreamSelection selection = selectStartupStreams(candidates);
    const size_t missing = std::numeric_limits<size_t>::max();
    AbstractStream *audio = selection.audio == missing ? nullptr : streams[selection.audio];
    AbstractStream *video = selection.video == missing ? nullptr : streams[selection.video];

    bool audioPrepared = false;
    bool videoPrepared = false;
    const bool throughMedia = startupPolicy.shouldPrefetchMediaChunk();
    if(audio && video)
    {
        StartupPreparationTask task = { audio, throughMedia, false };
        vlc_thread_t worker;
        if(vlc_clone(&worker, prepareStartupStream, &task) == 0)
        {
            videoPrepared = video->prepareStartupChunk(throughMedia);
            vlc_join(worker, nullptr);
            audioPrepared = task.succeeded;
        }
        else
        {
            audioPrepared = audio->prepareStartupChunk(throughMedia);
            videoPrepared = video->prepareStartupChunk(throughMedia);
        }
    }
    else if(audio)
        audioPrepared = audio->prepareStartupChunk(throughMedia);
    else if(video)
        videoPrepared = video->prepareStartupChunk(throughMedia);

    msg_Dbg(p_demux, "adaptive parallel startup prepared audio=%d video=%d media=%d",
            audioPrepared, videoPrepared, throughMedia);
}

AbstractStream::BufferingStatus PlaylistManager::bufferize(Times deadline,
                                                           vlc_tick_t i_min_buffering,
                                                           vlc_tick_t i_max_buffering,
                                                           vlc_tick_t i_target_buffering)
{
    prepareStartupStreams();

    AbstractStream::BufferingStatus i_return = AbstractStream::BufferingStatus::End;

    /* First reorder by status >> buffering level */
    std::vector<PrioritizedAbstractStream> prioritized_streams(streams.size());
    std::vector<PrioritizedAbstractStream>::iterator it = prioritized_streams.begin();
    for(AbstractStream *stream : streams)
    {
        PrioritizedAbstractStream &p = *it;
        p.st = stream;
        p.status = p.st->getBufferAndStatus(deadline, i_min_buffering, i_max_buffering, &p.demuxed_amount);
        ++it;
    }
    std::sort(prioritized_streams.begin(), prioritized_streams.end(), streamCompare);

    for(PrioritizedAbstractStream &pst : prioritized_streams)
    {
        AbstractStream *st = pst.st;

        if(!st->isValid())
            continue;

        if(st->esCount())
        {
            if (st->isDisabled() &&
                (!st->isSelected() || !reactivateStream(st)))
                  continue;
        }
        else
        {
            /* initial */
        }

        AbstractStream::BufferingStatus i_ret = st->bufferize(deadline,
                                                               i_min_buffering,
                                                               i_max_buffering,
                                                               i_target_buffering,
                                                               getActiveStreamsCount() <= 1);
        if(i_return != AbstractStream::BufferingStatus::Ongoing) /* Buffering streams need to keep going */
        {
            if(i_ret > i_return)
                i_return = i_ret;
        }

        /* Bail out, will start again (high prio could be same starving stream) */
        if( i_return == AbstractStream::BufferingStatus::Lessthanmin )
            break;
    }

    vlc_mutex_locker locker(&demux.lock);
    if(demux.times.continuous == VLC_TICK_INVALID &&
        /* don't wait minbuffer on simple discontinuity or restart */
       (demux.pcr_syncpoint == TimestampSynchronizationPoint::Discontinuity ||
        /* prevents starting before buffering is reached */
        i_return != AbstractStream::BufferingStatus::Lessthanmin ))
    {
        demux.times = getFirstTimes();
    }

    return i_return;
}

AbstractStream::Status PlaylistManager::dequeue(Times floor, Times *barrier)
{
    AbstractStream::Status i_return = AbstractStream::Status::Eof;

    const Times deadline = *barrier;

    for(AbstractStream *st : streams)
    {
        Times pcr;
        AbstractStream::Status i_ret = st->dequeue(deadline, &pcr);
        if( i_ret > i_return )
            i_return = i_ret;

        if( pcr.continuous > floor.continuous )
        {
            if( barrier->continuous > pcr.continuous )
                *barrier = pcr;
        }
    }

    return i_return;
}

StreamPosition PlaylistManager::getResumePosition() const
{
    vlc_mutex_locker locker(&demux.lock);
    StreamPosition pos;
    pos.times = demux.times;
    return pos;
}

Times PlaylistManager::getFirstTimes() const
{
    Times mindts;
    for(const AbstractStream *stream : streams)
    {
        const Times dts = stream->getFirstTimes();
        if(mindts.continuous == VLC_TICK_INVALID)
            mindts = dts;
        else if(dts.continuous != VLC_TICK_INVALID &&
                dts.continuous < mindts.continuous)
            mindts = dts;
    }
    return mindts;
}

unsigned PlaylistManager::getActiveStreamsCount() const
{
    // TODO improve
    unsigned count = 0;
    for(const AbstractStream* st : streams)
    {
        if(st->isValid() && !st->isDisabled())
            count++;
    }
    return count;
}

bool PlaylistManager::setPosition(vlc_tick_t mediatime, double pos, bool accurate)
{
    bool ret = true;
    bool hasValidStream = false;
    StreamPosition streampos;
    streampos.times = demux.firsttimes;
    if(streampos.times.continuous != VLC_TICK_INVALID)
        streampos.times.offsetBy(mediatime - streampos.times.segment.media);
    else
        streampos.times.segment.media = mediatime;
    streampos.pos = pos;
    for(int real = 0; real < 2; real++)
    {
        /* Always probe if we can seek first */
        for(AbstractStream* st : streams)
        {
            if(st->isValid() && !st->isDisabled())
            {
                hasValidStream = true;
                ret &= st->setPosition(streampos, !real);
            }
        }
        if(!ret)
            break;
    }
    if(!hasValidStream)
    {
        msg_Warn(p_demux, "there is no valid streams");
        ret = false;
    }

    if(accurate && ret && streampos.times.continuous >= VLC_TICK_0)
    {
        es_out_Control(p_demux->out, ES_OUT_SET_NEXT_DISPLAY_TIME,
                       streampos.times.continuous);
        SeekDebug(msg_Dbg(p_demux,"ES_OUT_SET_NEXT_DISPLAY_TIME to %" PRId64,
                          streampos.times.continuous));
    }

    return ret;
}

void PlaylistManager::setLivePause(bool b)
{
    if(!started())
        return;

    for(AbstractStream* st : streams)
        if(st->isValid() && !st->isDisabled())
            st->setLivePause(b);
}

bool PlaylistManager::needsUpdate() const
{
    return playlist->needsUpdates() &&
           playlist->isLive() && (failedupdates < 3);
}

void PlaylistManager::scheduleNextUpdate()
{

}

bool PlaylistManager::updatePlaylist()
{
    for(AbstractStream* st : streams)
        st->runUpdates();

    updateControlsPosition();
    return true;
}

void PlaylistManager::preparsePlaylist()
{

}

Times PlaylistManager::getTimes(bool b_first) const
{
    vlc_mutex_locker locker(&demux.lock);
    return b_first ? demux.firsttimes : demux.times;
}

vlc_tick_t PlaylistManager::getMinAheadTime() const
{
    vlc_tick_t minbuffer = 0;
    std::for_each(streams.cbegin(), streams.cend(),
        [&minbuffer](const AbstractStream *st) {
            if(st->isValid() && !st->isDisabled() && st->isSelected())
            {
                const vlc_tick_t m = st->getMinAheadTime();
                if(m > 0 && (m < minbuffer || minbuffer == 0))
                    minbuffer = m;
            }
        });
    return minbuffer;
}

bool PlaylistManager::reactivateStream(AbstractStream *stream)
{
    return stream->reactivate(getResumePosition());
}

#define DEMUX_INCREMENT VLC_TICK_FROM_MS(50)
int PlaylistManager::demux_callback(demux_t *p_demux)
{
    PlaylistManager *manager = reinterpret_cast<PlaylistManager *>(p_demux->p_sys);
    if(!manager->started() && !manager->start())
        return VLC_DEMUXER_EOF;
    return manager->doDemux(DEMUX_INCREMENT);
}

int PlaylistManager::doDemux(vlc_tick_t increment)
{
    vlc_mutex_lock(&demux.lock);
    if(demux.times.continuous == VLC_TICK_INVALID)
    {
        bool b_dead = true;
        bool b_all_disabled = true;
        std::vector<AbstractStream *>::const_iterator it;
        for(it=streams.begin(); it!=streams.end(); ++it)
        {
            b_dead &= !(*it)->isValid();
            b_all_disabled &= (*it)->isDisabled();
        }
        if(!b_dead)
            vlc_cond_timedwait(&demux.cond, &demux.lock, vlc_tick_now() + VLC_TICK_FROM_MS(50));
        vlc_mutex_unlock(&demux.lock);
        return (b_dead || b_all_disabled) ? VLC_DEMUXER_EOF : VLC_DEMUXER_SUCCESS;
    }

    Times barrier = demux.times;
    barrier.offsetBy(increment);

    vlc_mutex_unlock(&demux.lock);

    AbstractStream::Status status = dequeue(demux.times, &barrier);

    vlc_mutex_lock(&demux.lock);
    if(demux.firsttimes.continuous == VLC_TICK_INVALID && barrier.continuous != VLC_TICK_INVALID)
    {
        demux.firsttimes = barrier;
        assert(barrier.segment.media != VLC_TICK_INVALID);
    }
    vlc_mutex_unlock(&demux.lock);

    updateControlsPosition();

    switch(status)
    {
    case AbstractStream::Status::Eof:
        {
            /* might be end of current period */
            if(currentPeriod)
            {
                setBufferingRunState(false);
                BasePeriod *nextPeriod = playlist->getNextPeriod(currentPeriod);
                if(!nextPeriod)
                    return VLC_DEMUXER_EOF;
                unsetPeriod();
                currentPeriod = nextPeriod;
                if (!setupPeriod())
                    return VLC_DEMUXER_EOF;

                demux.times = Times();
                demux.firsttimes = Times();
                es_out_Control(p_demux->out, ES_OUT_RESET_PCR);

                setBufferingRunState(true);
            }
        }
        break;
    case AbstractStream::Status::Buffering:
        vlc_mutex_lock(&demux.lock);
        vlc_cond_timedwait(&demux.cond, &demux.lock, vlc_tick_now() + VLC_TICK_FROM_MS(50));
        vlc_mutex_unlock(&demux.lock);
        break;
    case AbstractStream::Status::Discontinuity:
        vlc_mutex_lock(&demux.lock);
        demux.times = Times();
        demux.firsttimes = Times();
        demux.pcr_syncpoint = TimestampSynchronizationPoint::Discontinuity;
        es_out_Control(p_demux->out, ES_OUT_RESET_PCR);
        vlc_mutex_unlock(&demux.lock);
        break;
    case AbstractStream::Status::Demuxed:
        {
            bool advanced = false;
            vlc_mutex_lock(&demux.lock);
            if( demux.times.continuous != VLC_TICK_INVALID && barrier.continuous != demux.times.continuous )
            {
                demux.times = barrier;
                vlc_tick_t pcr = VLC_TICK_0 + std::max(INT64_C(0), demux.times.continuous - VLC_TICK_FROM_MS(100));
                es_out_Control(p_demux->out, ES_OUT_SET_GROUP_PCR, 0, pcr);
                advanced = true;
            }
            vlc_mutex_unlock(&demux.lock);
            if(advanced && startupPolicy.isStarting())
            {
                resources->getConnManager()->setStartupConcurrency(false);
                startupPolicy.onMediaDequeued();
                msg_Dbg(p_demux, "adaptive fast start completed; restored normal buffering");
            }
            break;
        }
    }

    return VLC_DEMUXER_SUCCESS;
}

int PlaylistManager::control_callback(demux_t *p_demux, int i_query, va_list args)
{
    PlaylistManager *manager = reinterpret_cast<PlaylistManager *>(p_demux->p_sys);
    return manager->doControl(i_query, args);
}

int PlaylistManager::doControl(int i_query, va_list args)
{
    switch (i_query)
    {
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_PACE:
            *(va_arg (args, bool *)) = true;
            break;

        case DEMUX_CAN_PAUSE:
        {
            /* Always return true then fail late.
             * See demux.c/demux_vaControl,
             * misleading and should be DEMUX_CAN_CONTROL_PAUSE */
            *(va_arg (args, bool *)) = true;
            break;
        }

        case DEMUX_SET_PAUSE_STATE:
        {
            setBufferingRunState(false); /* /!\ always stop buffering process first */
            bool b_pause = (bool)va_arg(args, int);
            if(playlist->isLive())
            {
                vlc_tick_t now = vlc_tick_now();
                demux.times = Times();
                cached.lastupdate = 0;
                if(b_pause)
                {
                    setLivePause(true);
                    pause_start = now;
                    msg_Dbg(p_demux,"Buffering and playback paused. No timeshift support.");
                }
                else
                {
                    setLivePause(false);
                    msg_Dbg(p_demux,"Resuming buffering/playback after %" PRId64 "ms",
                            MS_FROM_VLC_TICK(now-pause_start));
                    es_out_Control(p_demux->out, ES_OUT_RESET_PCR);
                    setBufferingRunState(true);
                }
            }
            else setBufferingRunState(true);
            return VLC_SUCCESS;
        }

        case DEMUX_GET_TIME:
        {
            vlc_mutex_locker locker(&cached.lock);
            *(va_arg (args, vlc_tick_t *)) = cached.i_time;
            break;
        }

        case DEMUX_GET_NORMAL_TIME:
        {
            vlc_mutex_locker locker(&cached.lock);
            if( cached.i_normaltime == VLC_TICK_INVALID )
                return VLC_EGENERIC;
            *(va_arg (args, vlc_tick_t *)) = cached.i_normaltime;
            break;
        }

        case DEMUX_GET_LENGTH:
        {
            vlc_mutex_locker locker(&cached.lock);
            if(cached.b_live && cached.playlistLength == 0)
                return VLC_EGENERIC;
            *(va_arg (args, vlc_tick_t *)) = cached.playlistLength;
            *(va_arg (args, bool *)) = cached.b_live;
            break;
        }

        case DEMUX_GET_POSITION:
        {
            vlc_mutex_locker locker(&cached.lock);
            if(cached.b_live && cached.playlistLength == 0)
                return VLC_EGENERIC;
            *(va_arg (args, double *)) = cached.f_position;
            break;
        }

        case DEMUX_SET_POSITION:
        {
            setBufferingRunState(false); /* /!\ always stop buffering process first */
            vlc_mutex_locker locker(&cached.lock);

            if(cached.playlistLength == 0)
            {
                setBufferingRunState(true);
                return VLC_EGENERIC;
            }

            double pos = va_arg(args, double);
            bool accurate = va_arg(args, int);
            vlc_tick_t seekTime = cached.playlistStart + cached.playlistLength * pos;

            SeekDebug(msg_Dbg(p_demux, "Seek %f to %" PRId64 " plstart %" PRId64 " duration %" PRId64,
                   pos, seekTime, cached.playlistEnd, cached.playlistLength));

            if(!setPosition(seekTime, pos, accurate))
            {
                setBufferingRunState(true);
                return VLC_EGENERIC;
            }

            demux.pcr_syncpoint = TimestampSynchronizationPoint::RandomAccess;
            demux.times = Times();
            demux.firsttimes = Times();
            cached.lastupdate = 0;
            cached.i_normaltime = VLC_TICK_INVALID;
            cached.i_time = VLC_TICK_INVALID;
            setBufferingRunState(true);
            break;
        }

        case DEMUX_SET_TIME:
        {
            setBufferingRunState(false); /* stop downloader first */

            vlc_tick_t time = va_arg(args, vlc_tick_t);
            bool accurate = va_arg(args, int);
            if(!setPosition(time, -1, accurate))
            {
                setBufferingRunState(true);
                return VLC_EGENERIC;
            }

            vlc_mutex_locker locker(&cached.lock);
            demux.pcr_syncpoint = TimestampSynchronizationPoint::RandomAccess;
            demux.times = Times();
            demux.firsttimes = Times();
            cached.lastupdate = 0;
            cached.i_normaltime = VLC_TICK_INVALID;
            cached.i_time = VLC_TICK_INVALID;
            setBufferingRunState(true);
            break;
        }

        case DEMUX_GET_PTS_DELAY:
            *va_arg (args, vlc_tick_t *) = VLC_TICK_FROM_SEC(1);
            break;

        case DEMUX_GET_ADAPTIVE_VARIANTS:
        {
            vlc_adaptive_variant_t **pp_variants =
                va_arg(args, vlc_adaptive_variant_t **);
            size_t *p_count = va_arg(args, size_t *);
            return getAdaptiveVariants(pp_variants, p_count);
        }

        case DEMUX_SET_ADAPTIVE_VARIANT:
            return setAdaptiveVariant(va_arg(args, const char *));

        case DEMUX_GET_ADAPTIVE_VARIANT:
            return getAdaptiveVariant(va_arg(args, char **));

        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

void PlaylistManager::setBufferingRunState(bool b)
{
    mutex_locker locker {lock};
    b_buffering = b;
    waitcond.signal();
}

void PlaylistManager::Run()
{
    mutex_locker locker {lock};
    const vlc_tick_t i_upstream_min_buffering = bufferingLogic->getMinBuffering(playlist);
    const vlc_tick_t i_max_buffering = bufferingLogic->getMaxBuffering(playlist);
    const vlc_tick_t i_target_buffering = bufferingLogic->getStableBuffering(playlist);
    while(1)
    {
        while(!b_buffering && !b_canceled)
            waitcond.wait(lock);
        if (b_canceled)
            break;

        if(needsUpdate())
        {
            if(updatePlaylist())
                scheduleNextUpdate();
            else
                failedupdates++;
        }

        vlc_mutex_lock(&demux.lock);
        Times pcr = demux.times;
        vlc_mutex_unlock(&demux.lock);

        const vlc_tick_t i_min_buffering =
                startupPolicy.getMinBuffering(i_upstream_min_buffering);
        AbstractStream::BufferingStatus i_return = bufferize(pcr, i_min_buffering,
                                                             i_max_buffering, i_target_buffering);
        preparePendingAdaptiveSwitch();

        if(i_return != AbstractStream::BufferingStatus::Lessthanmin)
        {
            vlc_tick_t i_deadline = vlc_tick_now();
            if(i_return == AbstractStream::BufferingStatus::Ongoing)
                i_deadline += VLC_TICK_FROM_MS(10);
            else if(i_return == AbstractStream::BufferingStatus::Full)
                i_deadline += VLC_TICK_FROM_MS(100);
            else if(i_return == AbstractStream::BufferingStatus::End)
                i_deadline += VLC_TICK_FROM_SEC(1);
            else /*if(i_return == AbstractStream::BufferingStatus::suspended)*/
                i_deadline += VLC_TICK_FROM_MS(250);

            // TODO: The current function doesn't seem to modify shared
            //       state under demux lock.
            vlc_cond_signal(&demux.cond);

            while(b_buffering &&
                    waitcond.timedwait(lock, i_deadline) == 0 &&
                    i_deadline > vlc_tick_now() &&
                    !b_canceled);
            if (b_canceled)
                break;
        }
    }
}

void * PlaylistManager::managerThread(void *opaque)
{
    vlc_thread_set_name("vlc-adapt-mngr");
    static_cast<PlaylistManager *>(opaque)->Run();
    return nullptr;
}

void PlaylistManager::updateControlsPosition()
{
    vlc_mutex_locker locker(&cached.lock);

    time_t now = time(nullptr);
    if(now - cached.lastupdate < 1)
        return;
    cached.lastupdate = now;

    for(AbstractStream* st : streams)
    {
        if(st->isValid() && !st->isDisabled() && st->isSelected())
        {
            if(st->getMediaPlaybackTimes(&cached.playlistStart, &cached.playlistEnd,
                                         &cached.playlistLength))
                break;
        }
    }

    /*
     * Relative position:
     * -> Elapsed demux time (current demux time - first demux time)
     * Since PlaylistTime != DemuxTime (HLS crap, TS):
     * -> Use Playlist<->Demux time offset provided by EsOut
     *    to convert to elapsed playlist time.
     *    But this diff is not available until we have demuxed data...
     *    Fallback on relative seek from playlist start in that case
     * But also playback might not have started at beginning of playlist
     * -> Apply relative start from seek point (set on EsOut as ExpectedTime)
     *
     * All seeks need to be done in playlist time !
     */

    Times currentTimes = getTimes();
    Times startTimes = getTimes(true);
    cached.b_live = playlist->isLive();

    SeekDebug(msg_Dbg(p_demux, "playlist Start/End %" PRId64 "/%" PRId64 " len %" PRId64
                               "rap pl/demux (%" PRId64 "/%" PRId64 ")",
                      cached.playlistStart, cached.playlistEnd, cached.playlistEnd,
                      startTimes.segment.media, startTimes.segment.demux));

    if(cached.b_live)
    {
        /* Special case for live until we can provide relative start to fully match
           the above description */
        cached.i_time = currentTimes.segment.media;

        if(cached.playlistStart != cached.playlistEnd)
        {
            if(cached.playlistStart < 0) /* Live template. Range start = now() - buffering depth */
            {
                cached.playlistEnd = vlc_tick_from_sec(now);
                cached.playlistStart = cached.playlistEnd - cached.playlistLength;
            }
        }

        if(cached.i_time > VLC_TICK_0 + cached.playlistStart &&
           cached.i_time <= VLC_TICK_0 + cached.playlistEnd && cached.playlistLength)
        {
            cached.f_position = ((double)(cached.i_time - VLC_TICK_0 - cached.playlistStart)) / cached.playlistLength;
        }
        else
        {
            cached.f_position = 0.0;
        }
    }
    else
    {
        cached.i_normaltime = startTimes.continuous;
        cached.i_normaltime += VLC_TICK_0 + cached.playlistStart - startTimes.segment.media;

        if(playlist->duration > cached.playlistLength)
            cached.playlistLength = playlist->duration;

        if(cached.playlistLength && currentTimes.segment.media != VLC_TICK_INVALID)
        {
            cached.i_time = currentTimes.segment.media;
            cached.f_position = (double) (cached.i_time - VLC_TICK_0 - cached.playlistStart) / cached.playlistLength;
        }
        else
        {
            cached.f_position = 0.0;
        }
    }

    SeekDebug(msg_Dbg(p_demux, "cached.i_time (%" PRId64 ") cur %" PRId64 " rap start (pl %" PRId64 "/dmx %" PRId64 ") pos %f",
                      cached.i_time, currentTimes.continuous, startTimes.segment.media,
                            startTimes.segment.demux, cached.f_position));
}

AbstractAdaptationLogic *PlaylistManager::createLogic(AbstractAdaptationLogic::LogicType type, AbstractConnectionManager *conn)
{
    vlc_object_t *obj = VLC_OBJECT(p_demux);
    AbstractAdaptationLogic *logic = nullptr;
    switch(type)
    {
        case AbstractAdaptationLogic::LogicType::FixedRate:
        {
            size_t bps = var_InheritInteger(p_demux, "adaptive-bw") * 8192;
            logic = new (std::nothrow) FixedRateAdaptationLogic(obj, bps);
            break;
        }
        case AbstractAdaptationLogic::LogicType::AlwaysLowest:
            logic = new (std::nothrow) AlwaysLowestAdaptationLogic(obj);
            break;
        case AbstractAdaptationLogic::LogicType::AlwaysBest:
            logic = new (std::nothrow) AlwaysBestAdaptationLogic(obj);
            break;
        case AbstractAdaptationLogic::LogicType::RateBased:
        {
            RateBasedAdaptationLogic *ratelogic =
                    new (std::nothrow) RateBasedAdaptationLogic(obj);
            if(ratelogic)
                conn->setDownloadRateObserver(ratelogic);
            logic = ratelogic;
            break;
        }
        case AbstractAdaptationLogic::LogicType::Default:
#ifdef ADAPTIVE_DEBUGGING_LOGIC
            logic = new (std::nothrow) RoundRobinLogic(obj);
            msg_Warn(p_demux, "using RoundRobinLogic every %u", RoundRobinLogic::QUANTUM);
            break;
#endif
        case AbstractAdaptationLogic::LogicType::NearOptimal:
        {
            NearOptimalAdaptationLogic *noplogic =
                    new (std::nothrow) NearOptimalAdaptationLogic(obj);
            if(noplogic)
                conn->setDownloadRateObserver(noplogic);
            logic = noplogic;
            break;
        }
        case AbstractAdaptationLogic::LogicType::Predictive:
        {
            AbstractAdaptationLogic *predictivelogic =
                    new (std::nothrow) PredictiveAdaptationLogic(obj);
            if(predictivelogic)
                conn->setDownloadRateObserver(predictivelogic);
            logic = predictivelogic;
        }

        default:
            break;
    }

    if(logic)
    {
        int w = var_InheritInteger(p_demux, "adaptive-maxwidth");
        int h = var_InheritInteger(p_demux, "adaptive-maxheight");
        if(h == 0)
        {
            h = var_InheritInteger(p_demux, "preferred-resolution");
            /* Adapt for slightly different minimum/maximum semantics */
            if(h == -1)
                h = 0;
            else if(h == 0)
                h = 1;
        }

        logic->setMaxDeviceResolution(w, h);
    }

    return logic;
}

AbstractBufferingLogic *PlaylistManager::createBufferingLogic() const
{
    DefaultBufferingLogic *bl = new DefaultBufferingLogic();
    if(bl)
    {
        unsigned v = var_InheritInteger(p_demux, "adaptive-livedelay");
        if(v)
            bl->setUserLiveDelay(VLC_TICK_FROM_MS(v));
        v = var_InheritInteger(p_demux, "adaptive-maxbuffer");
        if(v)
            bl->setUserMaxBuffering(VLC_TICK_FROM_MS(v));
    }
    return bl;
}
