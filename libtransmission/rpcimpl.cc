// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include <libdeflate.h>

#include "libtransmission/transmission.h"

#include "libtransmission/announcer.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/quark.h"
#include "libtransmission/rpcimpl.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"
#include "libtransmission/version.h"
#include "libtransmission/web-utils.h"
#include "libtransmission/web.h"

using namespace std::literals;

namespace
{
auto constexpr RecentlyActiveSeconds = time_t{ 60 };
auto constexpr RpcVersion = int64_t{ 18 };
auto constexpr RpcVersionMin = int64_t{ 14 };
auto constexpr RpcVersionSemver = "5.4.0"sv;

enum class TrFormat
{
    Object,
    Table
};

// ---

/* For functions that can't be immediately executed, like torrentAdd,
 * this is the callback data used to pass a response to the caller
 * when the task is complete */
struct tr_rpc_idle_data
{
    std::optional<int64_t> tag;
    tr_session* session = nullptr;
    tr_variant::Map args_out = {};
    tr_rpc_response_func callback = nullptr;
    void* callback_user_data = nullptr;
};

auto constexpr SuccessResult = "success"sv;

void tr_idle_function_done(struct tr_rpc_idle_data* data, std::string_view result)
{
    // build the response
    auto response_map = tr_variant::Map{ 3U };
    response_map.try_emplace(TR_KEY_arguments, std::move(data->args_out));
    response_map.try_emplace(TR_KEY_result, result);
    if (data->tag)
    {
        response_map.try_emplace(TR_KEY_tag, *data->tag);
    }

    // send the response back to the listener
    auto response = tr_variant{ std::move(response_map) };
    (*data->callback)(data->session, &response, data->callback_user_data);

    // cleanup
    delete data;
}

// ---

[[nodiscard]] auto getTorrents(tr_session* session, tr_variant::Map const& args)
{
    auto torrents_vec = std::vector<tr_torrent*>{};

    auto& torrents = session->torrents();
    torrents_vec.reserve(std::size(torrents));
    auto const add_torrent_from_var = [&torrents, &torrents_vec](tr_variant const& var)
    {
        tr_torrent* tor = nullptr;

        if (auto const* val = var.get_if<int64_t>(); val != nullptr)
        {
            tor = torrents.get(*val);
        }

        if (auto const* val = var.get_if<std::string_view>(); val != nullptr)
        {
            if (*val == "recently-active"sv)
            {
                auto const cutoff = tr_time() - RecentlyActiveSeconds;

                std::copy_if(
                    std::begin(torrents),
                    std::end(torrents),
                    std::back_inserter(torrents_vec),
                    [cutoff](auto const* tor) { return tor->anyDate >= cutoff; });
            }
            else
            {
                tor = torrents.get(*val);
            }
        }

        if (tor != nullptr)
        {
            torrents_vec.push_back(tor);
        }
    };

    if (auto const ids_iter = args.find(TR_KEY_ids); ids_iter != std::end(args))
    {
        auto const& ids_var = ids_iter->second;

        if (auto const* ids_vec = ids_var.get_if<tr_variant::Vector>(); ids_vec != nullptr)
        {
            std::for_each(std::begin(*ids_vec), std::end(*ids_vec), add_torrent_from_var);
        }
        else
        {
            add_torrent_from_var(ids_var);
        }
    }
    else if (auto const id_iter = args.find(TR_KEY_ids); id_iter != std::end(args))
    {
        add_torrent_from_var(id_iter->second);
    }
    else // all of them
    {
        torrents_vec.assign(std::begin(torrents), std::end(torrents));
    }

    return torrents_vec;
}

void notifyBatchQueueChange(tr_session* session, std::vector<tr_torrent*> const& torrents)
{
    for (auto* tor : torrents)
    {
        session->rpcNotify(TR_RPC_TORRENT_CHANGED, tor);
    }

    session->rpcNotify(TR_RPC_SESSION_QUEUE_POSITIONS_CHANGED);
}

char const* queueMoveTop(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const torrents = getTorrents(session, args_in);
    tr_torrentsQueueMoveTop(std::data(torrents), std::size(torrents));
    notifyBatchQueueChange(session, torrents);
    return nullptr;
}

char const* queueMoveUp(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const torrents = getTorrents(session, args_in);
    tr_torrentsQueueMoveUp(std::data(torrents), std::size(torrents));
    notifyBatchQueueChange(session, torrents);
    return nullptr;
}

char const* queueMoveDown(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const torrents = getTorrents(session, args_in);
    tr_torrentsQueueMoveDown(std::data(torrents), std::size(torrents));
    notifyBatchQueueChange(session, torrents);
    return nullptr;
}

char const* queueMoveBottom(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const torrents = getTorrents(session, args_in);
    tr_torrentsQueueMoveBottom(std::data(torrents), std::size(torrents));
    notifyBatchQueueChange(session, torrents);
    return nullptr;
}

constexpr struct
{
    constexpr bool operator()(tr_torrent const* a, tr_torrent const* b) const
    {
        return a->queuePosition < b->queuePosition;
    }
} CompareTorrentByQueuePosition{};

char const* torrentStart(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto torrents = getTorrents(session, args_in);
    std::sort(std::begin(torrents), std::end(torrents), CompareTorrentByQueuePosition);
    for (auto* tor : torrents)
    {
        if (!tor->is_running())
        {
            tr_torrentStart(tor);
            session->rpcNotify(TR_RPC_TORRENT_STARTED, tor);
        }
    }

    return nullptr;
}

char const* torrentStartNow(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto torrents = getTorrents(session, args_in);
    std::sort(std::begin(torrents), std::end(torrents), CompareTorrentByQueuePosition);
    for (auto* tor : torrents)
    {
        if (!tor->is_running())
        {
            tr_torrentStartNow(tor);
            session->rpcNotify(TR_RPC_TORRENT_STARTED, tor);
        }
    }

    return nullptr;
}

char const* torrentStop(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    for (auto* tor : getTorrents(session, args_in))
    {
        if (tor->is_running() || tor->is_queued() || tor->verify_state() != TR_VERIFY_NONE)
        {
            tor->is_stopping_ = true;
            session->rpcNotify(TR_RPC_TORRENT_STOPPED, tor);
        }
    }

    return nullptr;
}

char const* torrentRemove(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const delete_flag = args_in.value_if<bool>(TR_KEY_delete_local_data).value_or(false);
    auto const type = delete_flag ? TR_RPC_TORRENT_TRASHING : TR_RPC_TORRENT_REMOVING;

    for (auto* tor : getTorrents(session, args_in))
    {
        if (auto const status = session->rpcNotify(type, tor); (status & TR_RPC_NOREMOVE) == 0)
        {
            tr_torrentRemove(tor, delete_flag, nullptr, nullptr);
        }
    }

    return nullptr;
}

char const* torrentReannounce(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    for (auto* tor : getTorrents(session, args_in))
    {
        if (tr_torrentCanManualUpdate(tor))
        {
            tr_torrentManualUpdate(tor);
            session->rpcNotify(TR_RPC_TORRENT_CHANGED, tor);
        }
    }

    return nullptr;
}

char const* torrentVerify(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    for (auto* tor : getTorrents(session, args_in))
    {
        tr_torrentVerify(tor);
        session->rpcNotify(TR_RPC_TORRENT_CHANGED, tor);
    }

    return nullptr;
}

// ---

namespace make_torrent_field_helpers
{
[[nodiscard]] auto make_file_wanted_vec(tr_torrent const& tor)
{
    auto const n_files = tor.file_count();
    auto priorities_vec = tr_variant::Vector{};
    priorities_vec.reserve(n_files);
    for (tr_file_index_t i = 0; i != n_files; ++i)
    {
        priorities_vec.emplace_back(tr_torrentFile(&tor, i).wanted ? 1 : 0);
    }
    return tr_variant{ std::move(priorities_vec) };
}

[[nodiscard]] auto make_labels_vec(tr_torrent const& tor)
{
    auto const n_labels = std::size(tor.labels);
    auto labels = tr_variant::Vector{};
    labels.reserve(n_labels);
    for (auto const& label : tor.labels)
    {
        labels.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(label)));
    }
    return tr_variant{ std::move(labels) };
}

[[nodiscard]] auto make_file_priorities_vec(tr_torrent const& tor)
{
    auto const n_files = tor.file_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_files);
    for (tr_file_index_t i = 0; i != n_files; ++i)
    {
        vec.emplace_back(tr_torrentFile(&tor, i).priority);
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_file_stats_vec(tr_torrent const& tor)
{
    auto const n_files = tor.file_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_files);
    for (tr_file_index_t i = 0; i != n_files; ++i)
    {
        auto const file = tr_torrentFile(&tor, i);
        auto stats_map = tr_variant::Map{ 3U };
        stats_map.try_emplace(TR_KEY_bytesCompleted, file.have);
        stats_map.try_emplace(TR_KEY_priority, file.priority);
        stats_map.try_emplace(TR_KEY_wanted, file.wanted);
        vec.emplace_back(std::move(stats_map));
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_file_vec(tr_torrent const& tor)
{
    auto const n_files = tor.file_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_files);
    for (tr_file_index_t i = 0; i != n_files; ++i)
    {
        auto const file = tr_torrentFile(&tor, i);
        auto file_map = tr_variant::Map{ 5U };
        file_map.try_emplace(TR_KEY_beginPiece, file.beginPiece);
        file_map.try_emplace(TR_KEY_bytesCompleted, file.have);
        file_map.try_emplace(TR_KEY_endPiece, file.endPiece);
        file_map.try_emplace(TR_KEY_length, file.length);
        file_map.try_emplace(TR_KEY_name, file.name);
        vec.emplace_back(std::move(file_map));
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_webseed_vec(tr_torrent const& tor)
{
    auto const n_webseeds = tor.webseed_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_webseeds);
    for (size_t i = 0, n = n_webseeds; i < n; ++i)
    {
        vec.emplace_back(tr_variant::unmanaged_string(tor.webseed(i)));
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_tracker_vec(tr_torrent const& tor)
{
    auto const n_trackers = tor.tracker_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_trackers);
    for (auto const& tracker : tor.announce_list())
    {
        auto tracker_map = tr_variant::Map{ 5U };
        tracker_map.try_emplace(TR_KEY_announce, tracker.announce.sv());
        tracker_map.try_emplace(TR_KEY_id, tracker.id);
        tracker_map.try_emplace(TR_KEY_scrape, tracker.scrape.sv());
        tracker_map.try_emplace(TR_KEY_sitename, tracker.sitename.sv());
        tracker_map.try_emplace(TR_KEY_tier, tracker.tier);
        vec.emplace_back(std::move(tracker_map));
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_tracker_stats_vec(tr_torrent const& tor)
{
    auto const n_trackers = tr_torrentTrackerCount(&tor);
    auto trackers_vec = tr_variant::Vector{};
    trackers_vec.reserve(n_trackers);
    for (size_t i = 0; i < n_trackers; ++i)
    {
        auto const tracker = tr_torrentTracker(&tor, i);
        auto tracker_map = tr_variant::Map{ 27U };
        tracker_map.try_emplace(TR_KEY_announce, tracker.announce);
        tracker_map.try_emplace(TR_KEY_announceState, static_cast<int64_t>(tracker.announceState));
        tracker_map.try_emplace(TR_KEY_downloadCount, tracker.downloadCount);
        tracker_map.try_emplace(TR_KEY_hasAnnounced, tracker.hasAnnounced);
        tracker_map.try_emplace(TR_KEY_hasScraped, tracker.hasScraped);
        tracker_map.try_emplace(TR_KEY_host, tracker.host_and_port);
        tracker_map.try_emplace(TR_KEY_id, tracker.id);
        tracker_map.try_emplace(TR_KEY_isBackup, tracker.isBackup);
        tracker_map.try_emplace(TR_KEY_lastAnnouncePeerCount, tracker.lastAnnouncePeerCount);
        tracker_map.try_emplace(TR_KEY_lastAnnounceResult, tracker.lastAnnounceResult);
        tracker_map.try_emplace(TR_KEY_lastAnnounceStartTime, tracker.lastAnnounceStartTime);
        tracker_map.try_emplace(TR_KEY_lastAnnounceSucceeded, tracker.lastAnnounceSucceeded);
        tracker_map.try_emplace(TR_KEY_lastAnnounceTime, tracker.lastAnnounceTime);
        tracker_map.try_emplace(TR_KEY_lastAnnounceTimedOut, tracker.lastAnnounceTimedOut);
        tracker_map.try_emplace(TR_KEY_lastScrapeResult, tracker.lastScrapeResult);
        tracker_map.try_emplace(TR_KEY_lastScrapeStartTime, tracker.lastScrapeStartTime);
        tracker_map.try_emplace(TR_KEY_lastScrapeSucceeded, tracker.lastScrapeSucceeded);
        tracker_map.try_emplace(TR_KEY_lastScrapeTime, tracker.lastScrapeTime);
        tracker_map.try_emplace(TR_KEY_lastScrapeTimedOut, tracker.lastScrapeTimedOut);
        tracker_map.try_emplace(TR_KEY_leecherCount, tracker.leecherCount);
        tracker_map.try_emplace(TR_KEY_nextAnnounceTime, tracker.nextAnnounceTime);
        tracker_map.try_emplace(TR_KEY_nextScrapeTime, tracker.nextScrapeTime);
        tracker_map.try_emplace(TR_KEY_scrape, tracker.scrape);
        tracker_map.try_emplace(TR_KEY_scrapeState, tracker.scrapeState);
        tracker_map.try_emplace(TR_KEY_seederCount, tracker.seederCount);
        tracker_map.try_emplace(TR_KEY_sitename, tracker.sitename);
        tracker_map.try_emplace(TR_KEY_tier, tracker.tier);
        trackers_vec.emplace_back(std::move(tracker_map));
    }
    return tr_variant{ std::move(trackers_vec) };
}

[[nodiscard]] auto make_peer_vec(tr_torrent const& tor)
{
    auto peer_count = size_t{};
    tr_peer_stat* peers = tr_torrentPeers(&tor, &peer_count);
    auto peers_vec = tr_variant::Vector{};
    peers_vec.reserve(peer_count);
    for (size_t i = 0; i < peer_count; ++i)
    {
        auto const& peer = peers[i];
        auto peer_map = tr_variant::Map{ 16U };
        peer_map.try_emplace(TR_KEY_address, peer.addr);
        peer_map.try_emplace(TR_KEY_clientIsChoked, peer.clientIsChoked);
        peer_map.try_emplace(TR_KEY_clientIsInterested, peer.clientIsInterested);
        peer_map.try_emplace(TR_KEY_clientName, peer.client);
        peer_map.try_emplace(TR_KEY_flagStr, peer.flagStr);
        peer_map.try_emplace(TR_KEY_isDownloadingFrom, peer.isDownloadingFrom);
        peer_map.try_emplace(TR_KEY_isEncrypted, peer.isEncrypted);
        peer_map.try_emplace(TR_KEY_isIncoming, peer.isIncoming);
        peer_map.try_emplace(TR_KEY_isUTP, peer.isUTP);
        peer_map.try_emplace(TR_KEY_isUploadingTo, peer.isUploadingTo);
        peer_map.try_emplace(TR_KEY_peerIsChoked, peer.peerIsChoked);
        peer_map.try_emplace(TR_KEY_peerIsInterested, peer.peerIsInterested);
        peer_map.try_emplace(TR_KEY_port, peer.port);
        peer_map.try_emplace(TR_KEY_progress, peer.progress);
        peer_map.try_emplace(TR_KEY_rateToClient, tr_toSpeedBytes(peer.rateToClient_KBps));
        peer_map.try_emplace(TR_KEY_rateToPeer, tr_toSpeedBytes(peer.rateToPeer_KBps));
        peers_vec.emplace_back(std::move(peer_map));
    }
    tr_torrentPeersFree(peers, peer_count);
    return tr_variant{ std::move(peers_vec) };
}

[[nodiscard]] auto make_peer_counts_map(tr_stat const& st)
{
    auto const& from = st.peersFrom;
    auto peer_counts_map = tr_variant::Map{ 7U };
    peer_counts_map.try_emplace(TR_KEY_fromCache, from[TR_PEER_FROM_RESUME]);
    peer_counts_map.try_emplace(TR_KEY_fromDht, from[TR_PEER_FROM_DHT]);
    peer_counts_map.try_emplace(TR_KEY_fromIncoming, from[TR_PEER_FROM_INCOMING]);
    peer_counts_map.try_emplace(TR_KEY_fromLpd, from[TR_PEER_FROM_LPD]);
    peer_counts_map.try_emplace(TR_KEY_fromLtep, from[TR_PEER_FROM_LTEP]);
    peer_counts_map.try_emplace(TR_KEY_fromPex, from[TR_PEER_FROM_PEX]);
    peer_counts_map.try_emplace(TR_KEY_fromTracker, from[TR_PEER_FROM_TRACKER]);
    return tr_variant{ std::move(peer_counts_map) };
}

[[nodiscard]] auto make_piece_availability_vec(tr_torrent const& tor)
{
    auto const n_pieces = tor.piece_count();
    auto vec = tr_variant::Vector{};
    vec.reserve(n_pieces);
    for (tr_piece_index_t piece = 0; piece < n_pieces; ++piece)
    {
        vec.emplace_back(tr_peerMgrPieceAvailability(&tor, piece));
    }
    return tr_variant{ std::move(vec) };
}

[[nodiscard]] auto make_piece_bitfield(tr_torrent const& tor)
{
    if (!tor.has_metainfo())
    {
        auto const bytes = tor.create_piece_bitfield();
        return tr_variant{ tr_base64_encode({ reinterpret_cast<char const*>(std::data(bytes)), std::size(bytes) }) };
    }

    return tr_variant::unmanaged_string(""sv);
}
} // namespace make_torrent_field_helpers

[[nodiscard]] auto constexpr isSupportedTorrentGetField(tr_quark key)
{
    switch (key)
    {
    case TR_KEY_activityDate:
    case TR_KEY_addedDate:
    case TR_KEY_availability:
    case TR_KEY_bandwidthPriority:
    case TR_KEY_comment:
    case TR_KEY_corruptEver:
    case TR_KEY_creator:
    case TR_KEY_dateCreated:
    case TR_KEY_desiredAvailable:
    case TR_KEY_doneDate:
    case TR_KEY_downloadDir:
    case TR_KEY_downloadLimit:
    case TR_KEY_downloadLimited:
    case TR_KEY_downloadedEver:
    case TR_KEY_editDate:
    case TR_KEY_error:
    case TR_KEY_errorString:
    case TR_KEY_eta:
    case TR_KEY_etaIdle:
    case TR_KEY_fileStats:
    case TR_KEY_file_count:
    case TR_KEY_files:
    case TR_KEY_group:
    case TR_KEY_hashString:
    case TR_KEY_haveUnchecked:
    case TR_KEY_haveValid:
    case TR_KEY_honorsSessionLimits:
    case TR_KEY_id:
    case TR_KEY_isFinished:
    case TR_KEY_isPrivate:
    case TR_KEY_isStalled:
    case TR_KEY_labels:
    case TR_KEY_leftUntilDone:
    case TR_KEY_magnetLink:
    case TR_KEY_manualAnnounceTime:
    case TR_KEY_maxConnectedPeers:
    case TR_KEY_metadataPercentComplete:
    case TR_KEY_name:
    case TR_KEY_peer_limit:
    case TR_KEY_peers:
    case TR_KEY_peersConnected:
    case TR_KEY_peersFrom:
    case TR_KEY_peersGettingFromUs:
    case TR_KEY_peersSendingToUs:
    case TR_KEY_percentComplete:
    case TR_KEY_percentDone:
    case TR_KEY_pieceCount:
    case TR_KEY_pieceSize:
    case TR_KEY_pieces:
    case TR_KEY_primary_mime_type:
    case TR_KEY_priorities:
    case TR_KEY_queuePosition:
    case TR_KEY_rateDownload:
    case TR_KEY_rateUpload:
    case TR_KEY_recheckProgress:
    case TR_KEY_secondsDownloading:
    case TR_KEY_secondsSeeding:
    case TR_KEY_seedIdleLimit:
    case TR_KEY_seedIdleMode:
    case TR_KEY_seedRatioLimit:
    case TR_KEY_seedRatioMode:
    case TR_KEY_sizeWhenDone:
    case TR_KEY_source:
    case TR_KEY_startDate:
    case TR_KEY_status:
    case TR_KEY_torrentFile:
    case TR_KEY_totalSize:
    case TR_KEY_trackerList:
    case TR_KEY_trackerStats:
    case TR_KEY_trackers:
    case TR_KEY_uploadLimit:
    case TR_KEY_uploadLimited:
    case TR_KEY_uploadRatio:
    case TR_KEY_uploadedEver:
    case TR_KEY_wanted:
    case TR_KEY_webseeds:
    case TR_KEY_webseedsSendingToUs:
        return true;

    default:
        return false;
    }
}

[[nodiscard]] tr_variant make_torrent_field(tr_torrent const& tor, tr_stat const& st, tr_quark key)
{
    using namespace make_torrent_field_helpers;

    TR_ASSERT(isSupportedTorrentGetField(key));

    // clang-format off
    switch (key)
    {
    case TR_KEY_activityDate: return st.activityDate;
    case TR_KEY_addedDate: return st.addedDate;
    case TR_KEY_availability: return make_piece_availability_vec(tor);
    case TR_KEY_bandwidthPriority: return static_cast<int64_t>(tor.get_priority());
    case TR_KEY_comment: return tor.comment();
    case TR_KEY_corruptEver: return st.corruptEver;
    case TR_KEY_creator: return tor.creator();
    case TR_KEY_dateCreated: return tor.date_created();
    case TR_KEY_desiredAvailable: return st.desiredAvailable;
    case TR_KEY_doneDate: return st.doneDate;
    case TR_KEY_downloadDir: return tor.download_dir().sv();
    case TR_KEY_downloadLimit: return static_cast<int64_t>(tr_torrentGetSpeedLimit_KBps(&tor, TR_DOWN));
    case TR_KEY_downloadLimited: return tor.uses_speed_limit(TR_DOWN);
    case TR_KEY_downloadedEver: return st.downloadedEver;
    case TR_KEY_editDate: return st.editDate;
    case TR_KEY_error: return static_cast<int64_t>(st.error);
    case TR_KEY_errorString: return st.errorString;
    case TR_KEY_eta: return st.eta;
    case TR_KEY_etaIdle: return st.etaIdle;
    case TR_KEY_fileStats: return make_file_stats_vec(tor);
    case TR_KEY_file_count: return static_cast<int64_t>(tor.file_count());
    case TR_KEY_files: return make_file_vec(tor);
    case TR_KEY_group: return tor.bandwidth_group().sv();
    case TR_KEY_hashString: return tor.info_hash_string().sv();
    case TR_KEY_haveUnchecked: return st.haveUnchecked;
    case TR_KEY_haveValid: return st.haveValid;
    case TR_KEY_honorsSessionLimits: return tor.uses_session_limits();
    case TR_KEY_id: return st.id;
    case TR_KEY_isFinished: return st.finished;
    case TR_KEY_isPrivate: return tor.is_private();
    case TR_KEY_isStalled: return st.isStalled;
    case TR_KEY_labels: return make_labels_vec(tor);
    case TR_KEY_leftUntilDone: return st.leftUntilDone;
    case TR_KEY_magnetLink: return tor.metainfo_.magnet();
    case TR_KEY_manualAnnounceTime: return tr_announcerNextManualAnnounce(&tor);
    case TR_KEY_maxConnectedPeers: return static_cast<int64_t>(tor.peer_limit());
    case TR_KEY_metadataPercentComplete: return st.metadataPercentComplete;
    case TR_KEY_name: return tor.name();
    case TR_KEY_peer_limit: return static_cast<int64_t>(tor.peer_limit());
    case TR_KEY_peers: return make_peer_vec(tor);
    case TR_KEY_peersConnected: return static_cast<int64_t>(st.peersConnected);
    case TR_KEY_peersFrom: return make_peer_counts_map(st);
    case TR_KEY_peersGettingFromUs: return st.peersGettingFromUs;
    case TR_KEY_peersSendingToUs: return st.peersSendingToUs;
    case TR_KEY_percentComplete: return st.percentComplete;
    case TR_KEY_percentDone: return st.percentDone;
    case TR_KEY_pieceCount: return static_cast<int64_t>(tor.piece_count());
    case TR_KEY_pieceSize: return static_cast<int64_t>(tor.piece_size());
    case TR_KEY_pieces: return make_piece_bitfield(tor);
    case TR_KEY_primary_mime_type: return tor.primary_mime_type();
    case TR_KEY_priorities: return make_file_priorities_vec(tor);
    case TR_KEY_queuePosition: return static_cast<int64_t>(st.queuePosition);
    case TR_KEY_rateDownload: return tr_toSpeedBytes(st.pieceDownloadSpeed_KBps);
    case TR_KEY_rateUpload: return tr_toSpeedBytes(st.pieceUploadSpeed_KBps);
    case TR_KEY_recheckProgress: return st.recheckProgress;
    case TR_KEY_secondsDownloading: return st.secondsDownloading;
    case TR_KEY_secondsSeeding: return st.secondsSeeding;
    case TR_KEY_seedIdleLimit: return tor.idle_limit_minutes();
    case TR_KEY_seedIdleMode: return static_cast<int64_t>(tor.idle_limit_mode());
    case TR_KEY_seedRatioLimit: return tr_torrentGetRatioLimit(&tor);
    case TR_KEY_seedRatioMode: return static_cast<int64_t>(tr_torrentGetRatioMode(&tor));
    case TR_KEY_sequentialDownload: return tor.is_sequential_download();
    case TR_KEY_sizeWhenDone: return st.sizeWhenDone;
    case TR_KEY_source: return tor.source();
    case TR_KEY_startDate: return st.startDate;
    case TR_KEY_status: return static_cast<int64_t>(st.activity);
    case TR_KEY_torrentFile: return tor.torrent_file();
    case TR_KEY_totalSize: return tor.total_size();
    case TR_KEY_trackerList: return tor.tracker_list();
    case TR_KEY_trackerStats: return make_tracker_stats_vec(tor);
    case TR_KEY_trackers: return make_tracker_vec(tor);
    case TR_KEY_uploadLimit: return static_cast<int64_t>(tr_torrentGetSpeedLimit_KBps(&tor, TR_UP));
    case TR_KEY_uploadLimited: return tor.uses_speed_limit(TR_UP);
    case TR_KEY_uploadRatio: return st.ratio;
    case TR_KEY_uploadedEver: return st.uploadedEver;
    case TR_KEY_wanted: return make_file_wanted_vec(tor);
    case TR_KEY_webseeds: return make_webseed_vec(tor);
    case TR_KEY_webseedsSendingToUs: return st.webseedsSendingToUs;
    default: return tr_variant{};
    }
    // clang-format on
}

[[nodiscard]] auto make_torrent_info_map(tr_torrent* const tor, tr_quark const* const fields, size_t const field_count)
{
    auto const* const st = tr_torrentStat(tor);
    auto info_map = tr_variant::Map{ field_count };
    for (size_t i = 0; i < field_count; ++i)
    {
        info_map.try_emplace(fields[i], make_torrent_field(*tor, *st, fields[i]));
    }
    return tr_variant{ std::move(info_map) };
}

[[nodiscard]] auto make_torrent_info_vec(tr_torrent* const tor, tr_quark const* const fields, size_t const field_count)
{
    auto const* const st = tr_torrentStat(tor);
    auto info_vec = tr_variant::Vector{};
    info_vec.reserve(field_count);
    for (size_t i = 0; i < field_count; ++i)
    {
        info_vec.emplace_back(make_torrent_field(*tor, *st, fields[i]));
    }
    return tr_variant{ std::move(info_vec) };
}

[[nodiscard]] auto make_torrent_info(
    tr_torrent* const tor,
    TrFormat const format,
    tr_quark const* const fields,
    size_t const field_count)
{
    return format == TrFormat::Table ? make_torrent_info_vec(tor, fields, field_count) :
                                       make_torrent_info_map(tor, fields, field_count);
}

char const* torrentGet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& args_out,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const torrents = getTorrents(session, args_in);
    auto torrents_vec = tr_variant::Vector{};

    auto const format = args_in.value_if<std::string_view>(TR_KEY_format).value_or("object"sv) == "table"sv ? TrFormat::Table :
                                                                                                              TrFormat::Object;

    if (args_in.value_if<std::string_view>(TR_KEY_ids).value_or(""sv) == "recently-active"sv)
    {
        auto const cutoff = tr_time() - RecentlyActiveSeconds;
        auto const ids = session->torrents().removedSince(cutoff);

        auto removed_vec = tr_variant::Vector{};
        removed_vec.reserve(std::size(ids));
        for (auto const& id : ids)
        {
            removed_vec.emplace_back(id);
        }
        args_out.try_emplace(TR_KEY_removed, std::move(removed_vec));
    }

    auto keys = std::vector<tr_quark>{};
    auto const* const fields_vec = args_in.find_if<tr_variant::Vector>(TR_KEY_fields);
    if (fields_vec != nullptr)
    {
        auto const n_fields = std::size(*fields_vec);
        keys.reserve(n_fields);
        for (auto const& field : *fields_vec)
        {
            if (auto const* field_sv = field.get_if<std::string_view>(); field_sv != nullptr)
            {
                if (auto const key = tr_quark_lookup(*field_sv); key && isSupportedTorrentGetField(*key))
                {
                    keys.emplace_back(*key);
                }
            }
        }
    }

    if (std::empty(keys))
    {
        return "no fields specified";
    }

    if (format == TrFormat::Table)
    {
        /* first entry is an array of property names */
        auto names = tr_variant::Vector{};
        names.reserve(std::size(keys));
        std::transform(
            std::begin(keys),
            std::end(keys),
            std::back_inserter(names),
            [](tr_quark key) { return tr_quark_get_string_view(key); });
        torrents_vec.emplace_back(std::move(names));
    }

    for (auto* const tor : torrents)
    {
        torrents_vec.emplace_back(make_torrent_info(tor, format, std::data(keys), std::size(keys)));
    }

    return nullptr; // no error message
}

// ---

[[nodiscard]] std::pair<std::vector<tr_quark>, char const* /*errmsg*/> make_labels(tr_variant::Vector const& labels_vec)
{
    auto const n_labels = std::size(labels_vec);

    auto labels = std::vector<tr_quark>{};
    labels.reserve(n_labels);
    for (auto const& label_var : labels_vec)
    {
        if (auto const* value = label_var.get_if<std::string_view>(); value != nullptr)
        {
            auto const label = tr_strv_strip(*value);

            if (std::empty(label))
            {
                return { {}, "labels cannot be empty" };
            }

            if (tr_strv_contains(label, ','))
            {
                return { {}, "labels cannot contain comma (,) character" };
            }

            labels.emplace_back(tr_quark_new(label));
        }
    }

    return { labels, nullptr };
}

char const* set_labels(tr_torrent* tor, tr_variant::Vector const& list)
{
    auto [labels, errmsg] = make_labels(list);

    if (errmsg != nullptr)
    {
        return errmsg;
    }

    tor->setLabels(labels);
    return nullptr;
}

[[nodiscard]] std::pair<char const*, std::vector<tr_file_index_t>> get_file_indices(
    tr_torrent const* tor,
    tr_variant::Vector const& files_vec)
{
    auto const n_files = tor->file_count();

    auto files = std::vector<tr_file_index_t>{};
    files.reserve(n_files);

    if (std::empty(files_vec)) // if empty set, apply to all
    {
        files.resize(n_files);
        std::iota(std::begin(files), std::end(files), 0);
    }
    else
    {
        for (auto const& file_var : files_vec)
        {
            if (auto const* val = file_var.get_if<int64_t>(); val != nullptr)
            {
                if (auto const idx = static_cast<tr_file_index_t>(*val); idx < n_files)
                {
                    files.push_back(idx);
                }
                else
                {
                    return { "file index out of range", {} };
                }
            }
        }
    }

    return { nullptr, std::move(files) };
}

char const* set_file_priorities(tr_torrent* tor, tr_priority_t priority, tr_variant::Vector const& files_vec)
{
    auto const [errmsg, indices] = get_file_indices(tor, files_vec);
    if (errmsg != nullptr)
    {
        return errmsg;
    }

    tor->set_file_priorities(std::data(indices), std::size(indices), priority);
    return nullptr; // no error
}

[[nodiscard]] char const* set_file_dls(tr_torrent* tor, bool wanted, tr_variant::Vector const& files_vec)
{
    auto const [errmsg, indices] = get_file_indices(tor, files_vec);
    if (errmsg != nullptr)
    {
        return errmsg;
    }
    tor->set_files_wanted(std::data(indices), std::size(indices), wanted);
    return nullptr; // no error
}

char const* add_tracker_urls(tr_torrent* tor, tr_variant::Vector const& urls_vec)
{
    auto const old_size = tor->tracker_count();

    for (auto const& url_var : urls_vec)
    {
        if (auto const* val = url_var.get_if<std::string_view>(); val != nullptr)
        {
            tor->announce_list().add(*val);
        }
    }

    if (tor->tracker_count() == old_size)
    {
        return "error setting announce list";
    }

    tor->announce_list().save(tor->torrent_file());
    tor->on_announce_list_changed();
    return nullptr; // no error
}

[[nodiscard]] char const* replace_trackers(tr_torrent* tor, tr_variant::Vector const& urls_vec)
{
    auto changed = false;

    for (size_t i = 0, vec_size = std::size(urls_vec); i + 1 < vec_size; i += 2U)
    {
        auto const* id_val = urls_vec[i].get_if<int64_t>();
        auto const* url_val = urls_vec[i + 1U].get_if<std::string_view>();

        if (id_val != nullptr && url_val != nullptr)
        {
            changed |= tor->announce_list().replace(static_cast<tr_tracker_id_t>(*id_val), *url_val);
        }
    }

    if (!changed)
    {
        return "error setting announce list";
    }

    tor->announce_list().save(tor->torrent_file());
    tor->on_announce_list_changed();
    return nullptr; // no error
}

[[nodiscard]] char const* remove_trackers(tr_torrent* tor, tr_variant::Vector const& ids_vec)
{
    auto const old_size = tor->tracker_count();

    for (auto const& id_var : ids_vec)
    {
        if (auto const* val = id_var.get_if<int64_t>(); val != nullptr)
        {
            tor->announce_list().remove(static_cast<tr_tracker_id_t>(*val));
        }
    }

    if (tor->tracker_count() == old_size)
    {
        return "error setting announce list";
    }

    tor->announce_list().save(tor->torrent_file());
    tor->on_announce_list_changed();
    return nullptr; // no error
}

char const* torrentSet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    char const* errmsg = nullptr;

    for (auto* tor : getTorrents(session, args_in))
    {
        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_bandwidthPriority); val != nullptr)
        {
            if (auto const priority = static_cast<tr_priority_t>(*val); tr_isPriority(priority))
            {
                tr_torrentSetPriority(tor, priority);
            }
        }

        if (auto const* val = args_in.find_if<std::string_view>(TR_KEY_group); val != nullptr)
        {
            tor->set_bandwidth_group(*val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_labels); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_labels(tor, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_files_unwanted); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_file_dls(tor, false, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_files_wanted); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_file_dls(tor, false, *val);
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_peer_limit); val != nullptr)
        {
            tr_torrentSetPeerLimit(tor, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_high); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_file_priorities(tor, TR_PRI_HIGH, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_low); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_file_priorities(tor, TR_PRI_LOW, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_normal); val != nullptr && errmsg == nullptr)
        {
            errmsg = set_file_priorities(tor, TR_PRI_NORMAL, *val);
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_downloadLimit); val != nullptr)
        {
            tr_torrentSetSpeedLimit_KBps(tor, TR_DOWN, *val);
        }

        if (auto const* val = args_in.find_if<bool>(TR_KEY_sequentialDownload); val != nullptr)
        {
            tor->set_sequential_download(*val);
        }

        if (auto const* val = args_in.find_if<bool>(TR_KEY_downloadLimited); val != nullptr)
        {
            tor->use_speed_limit(TR_DOWN, *val);
        }

        if (auto const* val = args_in.find_if<bool>(TR_KEY_honorsSessionLimits); val != nullptr)
        {
            tr_torrentUseSessionLimits(tor, *val);
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_uploadLimit); val != nullptr)
        {
            tr_torrentSetSpeedLimit_KBps(tor, TR_UP, *val);
        }

        if (auto const* val = args_in.find_if<bool>(TR_KEY_uploadLimited); val != nullptr)
        {
            tor->use_speed_limit(TR_UP, *val);
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_seedIdleLimit); val != nullptr)
        {
            tor->set_idle_limit(static_cast<uint16_t>(*val));
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_seedIdleMode); val != nullptr)
        {
            tr_torrentSetIdleMode(tor, static_cast<tr_idlelimit>(*val));
        }

        if (auto const* val = args_in.find_if<double>(TR_KEY_seedRatioLimit); val != nullptr)
        {
            tr_torrentSetRatioLimit(tor, *val);
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_seedRatioMode); val != nullptr)
        {
            tor->set_ratio_mode(static_cast<tr_ratiolimit>(*val));
        }

        if (auto const* val = args_in.find_if<int64_t>(TR_KEY_queuePosition); val != nullptr)
        {
            tr_torrentSetQueuePosition(tor, static_cast<size_t>(*val));
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_trackerAdd); val != nullptr)
        {
            errmsg = add_tracker_urls(tor, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_trackerRemove); val != nullptr)
        {
            errmsg = remove_trackers(tor, *val);
        }

        if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_trackerReplace); val != nullptr)
        {
            errmsg = replace_trackers(tor, *val);
        }

        if (auto const* val = args_in.find_if<std::string_view>(TR_KEY_trackerList); val != nullptr)
        {
            if (!tor->set_tracker_list(*val))
            {
                errmsg = "Invalid tracker list";
            }
        }

        session->rpcNotify(TR_RPC_TORRENT_CHANGED, tor);
    }

    return errmsg;
}

char const* torrentSetLocation(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const* const location = args_in.find_if<std::string_view>(TR_KEY_location);
    if (location == nullptr)
    {
        return "no location";
    }

    if (tr_sys_path_is_relative(*location))
    {
        return "new location path is not absolute";
    }

    auto const move_flag = args_in.value_if<bool>(TR_KEY_move).value_or(false);
    for (auto* tor : getTorrents(session, args_in))
    {
        tor->set_location(*location, move_flag, nullptr, nullptr);
        session->rpcNotify(TR_RPC_TORRENT_MOVED, tor);
    }

    return nullptr;
}

// ---

void torrentRenamePathDone(tr_torrent* tor, char const* oldpath, char const* newname, int error, void* user_data)
{
    auto* const data = static_cast<struct tr_rpc_idle_data*>(user_data);

    data->args_out.try_emplace(TR_KEY_id, tr_torrentId(tor));
    data->args_out.try_emplace(TR_KEY_path, oldpath);
    data->args_out.try_emplace(TR_KEY_name, newname);

    tr_idle_function_done(data, error != 0 ? tr_strerror(error) : SuccessResult);
}

char const* torrentRenamePath(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    struct tr_rpc_idle_data* idle_data)
{
    auto const torrents = getTorrents(session, args_in);
    if (std::size(torrents) != 1U)
    {
        return "torrent-rename-path requires 1 torrent";
    }

    auto const oldpath = args_in.value_if<std::string_view>(TR_KEY_path).value_or(""sv);
    auto const newname = args_in.value_if<std::string_view>(TR_KEY_name).value_or(""sv);
    torrents[0]->rename_path(oldpath, newname, torrentRenamePathDone, idle_data);
    return nullptr; // no error
}

// ---

void onPortTested(tr_web::FetchResponse const& web_response)
{
    auto const& [status, body, did_connect, did_timeout, user_data] = web_response;
    auto* data = static_cast<struct tr_rpc_idle_data*>(user_data);

    if (status != 200)
    {
        tr_idle_function_done(
            data,
            fmt::format(
                _("Couldn't test port: {error} ({error_code})"),
                fmt::arg("error", tr_webGetResponseStr(status)),
                fmt::arg("error_code", status)));
    }
    else // success
    {
        data->args_out.try_emplace(TR_KEY_port_is_open, tr_strv_starts_with(body, '1'));
        tr_idle_function_done(data, SuccessResult);
    }
}

char const* portTest(
    tr_session* session,
    tr_variant::Map const& /*args_in*/,
    tr_variant::Map& /*args_out*/,
    struct tr_rpc_idle_data* idle_data)
{
    auto const port = session->advertisedPeerPort();
    auto const url = fmt::format(FMT_STRING("https://portcheck.transmissionbt.com/{:d}"), port.host());
    session->fetch({ url, onPortTested, idle_data });
    return nullptr;
}

// ---

void onBlocklistFetched(tr_web::FetchResponse const& web_response)
{
    auto const& [status, body, did_connect, did_timeout, user_data] = web_response;
    auto* data = static_cast<struct tr_rpc_idle_data*>(user_data);
    auto* const session = data->session;

    if (status != 200)
    {
        // we failed to download the blocklist...
        tr_idle_function_done(
            data,
            fmt::format(
                _("Couldn't fetch blocklist: {error} ({error_code})"),
                fmt::arg("error", tr_webGetResponseStr(status)),
                fmt::arg("error_code", status)));
        return;
    }

    // see if we need to decompress the content
    auto content = std::vector<char>{};
    content.resize(1024 * 128);
    for (;;)
    {
        auto decompressor = std::unique_ptr<libdeflate_decompressor, void (*)(libdeflate_decompressor*)>{
            libdeflate_alloc_decompressor(),
            libdeflate_free_decompressor
        };
        auto actual_size = size_t{};
        auto const decompress_result = libdeflate_gzip_decompress(
            decompressor.get(),
            std::data(body),
            std::size(body),
            std::data(content),
            std::size(content),
            &actual_size);
        if (decompress_result == LIBDEFLATE_INSUFFICIENT_SPACE)
        {
            // need a bigger buffer
            content.resize(content.size() * 2);
            continue;
        }
        if (decompress_result == LIBDEFLATE_BAD_DATA)
        {
            // couldn't decompress it; maybe we downloaded an uncompressed file
            content.assign(std::begin(body), std::end(body));
        }
        break;
    }

    // tr_blocklistSetContent needs a source file,
    // so save content into a tmpfile
    auto const filename = tr_pathbuf{ session->configDir(), "/blocklist.tmp"sv };
    if (tr_error* error = nullptr; !tr_file_save(filename, content, &error))
    {
        tr_idle_function_done(
            data,
            fmt::format(
                _("Couldn't save '{path}': {error} ({error_code})"),
                fmt::arg("path", filename),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code)));
        tr_error_clear(&error);
        return;
    }

    // feed it to the session and give the client a response
    data->args_out.try_emplace(TR_KEY_blocklist_size, static_cast<int64_t>(tr_blocklistSetContent(session, filename)));
    tr_sys_path_remove(filename);
    tr_idle_function_done(data, SuccessResult);
}

char const* blocklistUpdate(
    tr_session* session,
    tr_variant::Map const& /*args_in*/,
    tr_variant::Map& /*args_out*/,
    struct tr_rpc_idle_data* idle_data)
{
    session->fetch({ session->blocklistUrl(), onBlocklistFetched, idle_data });
    return nullptr;
}

// ---

void addTorrentImpl(struct tr_rpc_idle_data* data, tr_ctor* ctor)
{
    tr_torrent* duplicate_of = nullptr;
    tr_torrent* tor = tr_torrentNew(ctor, &duplicate_of);
    tr_ctorFree(ctor);

    if (tor == nullptr && duplicate_of == nullptr)
    {
        tr_idle_function_done(data, "invalid or corrupt torrent file"sv);
        return;
    }

    static auto constexpr Fields = std::array<tr_quark, 3>{ TR_KEY_id, TR_KEY_name, TR_KEY_hashString };
    if (duplicate_of != nullptr)
    {
        data->args_out.try_emplace(
            TR_KEY_torrent_duplicate,
            make_torrent_info(duplicate_of, TrFormat::Object, std::data(Fields), std::size(Fields)));
        return;
    }

    data->session->rpcNotify(TR_RPC_TORRENT_ADDED, tor);
    data->args_out.try_emplace(
        TR_KEY_torrent_added,
        make_torrent_info(tor, TrFormat::Object, std::data(Fields), std::size(Fields)));
    tr_idle_function_done(data, SuccessResult);
}

struct add_torrent_idle_data
{
    struct tr_rpc_idle_data* data;
    tr_ctor* ctor;
};

void onMetadataFetched(tr_web::FetchResponse const& web_response)
{
    auto const& [status, body, did_connect, did_timeout, user_data] = web_response;
    auto* data = static_cast<struct add_torrent_idle_data*>(user_data);

    tr_logAddTrace(fmt::format(
        "torrentAdd: HTTP response code was {} ({}); response length was {} bytes",
        status,
        tr_webGetResponseStr(status),
        std::size(body)));

    if (status == 200 || status == 221) /* http or ftp success.. */
    {
        tr_ctorSetMetainfo(data->ctor, std::data(body), std::size(body), nullptr);
        addTorrentImpl(data->data, data->ctor);
    }
    else
    {
        tr_idle_function_done(
            data->data,
            fmt::format(
                _("Couldn't fetch torrent: {error} ({error_code})"),
                fmt::arg("error", tr_webGetResponseStr(status)),
                fmt::arg("error_code", status)));
    }

    delete data;
}

bool isCurlURL(std::string_view url)
{
    auto constexpr Schemes = std::array<std::string_view, 4>{ "http"sv, "https"sv, "ftp"sv, "sftp"sv };
    auto const parsed = tr_urlParse(url);
    return parsed && std::find(std::begin(Schemes), std::end(Schemes), parsed->scheme) != std::end(Schemes);
}

[[nodiscard]] auto file_list_from_list(tr_variant::Vector const& idx_vec)
{
    auto const n_files = std::size(idx_vec);
    auto files = std::vector<tr_file_index_t>{};
    files.reserve(n_files);
    for (auto const& idx_var : idx_vec)
    {
        if (auto const* val = idx_var.get_if<int64_t>(); val != nullptr)
        {
            files.emplace_back(static_cast<tr_file_index_t>(*val));
        }
    }
    return files;
}

char const* torrentAdd(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* idle_data)
{
    TR_ASSERT(idle_data != nullptr);

    auto const filename = args_in.value_if<std::string_view>(TR_KEY_filename).value_or(""sv);
    auto const metainfo_base64 = args_in.value_if<std::string_view>(TR_KEY_metainfo).value_or(""sv);
    if (std::empty(filename) && std::empty(metainfo_base64))
    {
        return "no filename or metainfo specified";
    }

    auto const download_dir = args_in.value_if<std::string_view>(TR_KEY_download_dir);
    if (download_dir && tr_sys_path_is_relative(*download_dir))
    {
        return "download directory path is not absolute";
    }

    auto* const ctor = tr_ctorNew(session);

    /* set the optional arguments */

    auto const cookies = args_in.value_if<std::string_view>(TR_KEY_cookies).value_or(""sv);

    if (download_dir && !std::empty(*download_dir))
    {
        auto const sz_download_dir = std::string{ *download_dir };
        tr_ctorSetDownloadDir(ctor, TR_FORCE, sz_download_dir.c_str());
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_paused); val != nullptr)
    {
        tr_ctorSetPaused(ctor, TR_FORCE, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_peer_limit); val != nullptr)
    {
        tr_ctorSetPeerLimit(ctor, TR_FORCE, static_cast<uint64_t>(*val));
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_bandwidthPriority); val != nullptr)
    {
        tr_ctorSetBandwidthPriority(ctor, static_cast<tr_priority_t>(*val));
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_files_unwanted); val != nullptr)
    {
        auto const files = file_list_from_list(*val);
        tr_ctorSetFilesWanted(ctor, std::data(files), std::size(files), false);
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_files_wanted); val != nullptr)
    {
        auto const files = file_list_from_list(*val);
        tr_ctorSetFilesWanted(ctor, std::data(files), std::size(files), true);
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_low); val != nullptr)
    {
        auto const files = file_list_from_list(*val);
        tr_ctorSetFilePriorities(ctor, std::data(files), std::size(files), TR_PRI_LOW);
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_normal); val != nullptr)
    {
        auto const files = file_list_from_list(*val);
        tr_ctorSetFilePriorities(ctor, std::data(files), std::size(files), TR_PRI_NORMAL);
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_priority_high); val != nullptr)
    {
        auto const files = file_list_from_list(*val);
        tr_ctorSetFilePriorities(ctor, std::data(files), std::size(files), TR_PRI_HIGH);
    }

    if (auto const* val = args_in.find_if<tr_variant::Vector>(TR_KEY_labels); val != nullptr)
    {
        auto const [labels, errmsg] = make_labels(*val);
        if (errmsg != nullptr)
        {
            tr_ctorFree(ctor);
            return errmsg;
        }

        tr_ctorSetLabels(ctor, std::data(labels), std::size(labels));
    }

    tr_logAddTrace(fmt::format("torrentAdd: filename is '{}'", filename));

    if (isCurlURL(filename))
    {
        auto* const d = new add_torrent_idle_data{ idle_data, ctor };
        auto options = tr_web::FetchOptions{ filename, onMetadataFetched, d };
        options.cookies = cookies;
        session->fetch(std::move(options));
    }
    else
    {
        auto ok = false;

        if (std::empty(filename))
        {
            auto const metainfo = tr_base64_decode(metainfo_base64);
            ok = tr_ctorSetMetainfo(ctor, std::data(metainfo), std::size(metainfo), nullptr);
        }
        else if (tr_sys_path_exists(tr_pathbuf{ filename }))
        {
            ok = tr_ctorSetMetainfoFromFile(ctor, filename);
        }
        else
        {
            ok = tr_ctorSetMetainfoFromMagnetLink(ctor, filename);
        }

        if (!ok)
        {
            return "unrecognized info";
        }

        addTorrentImpl(idle_data, ctor);
    }

    return nullptr;
}

// ---

void add_strings_from_var(std::set<std::string_view>& strings, tr_variant const& var)
{
    if (auto const* val = var.get_if<std::string_view>(); val != nullptr)
    {
        strings.insert(*val);
        return;
    }

    if (auto const* val = var.get_if<tr_variant::Vector>(); val != nullptr)
    {
        for (auto const& vecvar : *val)
        {
            add_strings_from_var(strings, vecvar);
        }
    }
}

[[nodiscard]] char const* groupGet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& args_out,
    struct tr_rpc_idle_data* /*idle_data*/)
{
    auto names = std::set<std::string_view>{};
    if (auto const iter = args_in.find(TR_KEY_name); iter != std::end(args_in))
    {
        add_strings_from_var(names, iter->second);
    }

    auto groups_vec = tr_variant::Vector{};
    for (auto const& [name, group] : session->bandwidthGroups())
    {
        if (names.empty() || names.count(name.sv()) > 0U)
        {
            auto const limits = group->get_limits();
            auto group_map = tr_variant::Map{ 6U };
            group_map.try_emplace(TR_KEY_honorsSessionLimits, group->are_parent_limits_honored(TR_UP));
            group_map.try_emplace(TR_KEY_name, name.sv());
            group_map.try_emplace(TR_KEY_speed_limit_down, limits.down_limit_KBps);
            group_map.try_emplace(TR_KEY_speed_limit_down_enabled, limits.down_limited);
            group_map.try_emplace(TR_KEY_speed_limit_up, limits.up_limit_KBps);
            group_map.try_emplace(TR_KEY_speed_limit_up_enabled, limits.up_limited);
            groups_vec.emplace_back(std::move(group_map));
        }
    }
    args_out.try_emplace(TR_KEY_group, std::move(groups_vec));

    return nullptr;
}

char const* groupSet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    struct tr_rpc_idle_data* /*idle_data*/)
{
    auto const name = tr_strv_strip(args_in.value_if<std::string_view>(TR_KEY_name).value_or(""sv));
    if (std::empty(name))
    {
        return "No group name given";
    }

    auto& group = session->getBandwidthGroup(name);
    auto limits = group.get_limits();

    if (auto const* const val = args_in.find_if<bool>(TR_KEY_speed_limit_down_enabled); val != nullptr)
    {
        limits.down_limited = *val;
    }

    if (auto const* const val = args_in.find_if<bool>(TR_KEY_speed_limit_up_enabled); val != nullptr)
    {
        limits.up_limited = *val;
    }

    if (auto const* const val = args_in.find_if<int64_t>(TR_KEY_speed_limit_down); val != nullptr)
    {
        limits.down_limit_KBps = static_cast<tr_kilobytes_per_second_t>(*val);
    }

    if (auto const* const val = args_in.find_if<int64_t>(TR_KEY_speed_limit_up); val != nullptr)
    {
        limits.up_limit_KBps = static_cast<tr_kilobytes_per_second_t>(*val);
    }

    group.set_limits(&limits);

    if (auto const* const val = args_in.find_if<bool>(TR_KEY_honorsSessionLimits); val != nullptr)
    {
        group.honor_parent_limits(TR_UP, *val);
        group.honor_parent_limits(TR_DOWN, *val);
    }

    return nullptr;
}

// ---

char const* sessionSet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const download_dir = args_in.value_if<std::string_view>(TR_KEY_download_dir);
    if (download_dir && tr_sys_path_is_relative(*download_dir))
    {
        return "download directory path is not absolute";
    }

    auto const incomplete_dir = args_in.value_if<std::string_view>(TR_KEY_incomplete_dir);
    if (incomplete_dir && tr_sys_path_is_relative(*incomplete_dir))
    {
        return "incomplete torrents directory path is not absolute";
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_cache_size_mb); val != nullptr)
    {
        tr_sessionSetCacheLimit_MB(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_alt_speed_up); val != nullptr)
    {
        tr_sessionSetAltSpeed_KBps(session, TR_UP, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_alt_speed_down); val != nullptr)
    {
        tr_sessionSetAltSpeed_KBps(session, TR_DOWN, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_alt_speed_enabled); val != nullptr)
    {
        tr_sessionUseAltSpeed(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_alt_speed_time_begin); val != nullptr)
    {
        tr_sessionSetAltSpeedBegin(session, static_cast<size_t>(*val));
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_alt_speed_time_end); val != nullptr)
    {
        tr_sessionSetAltSpeedEnd(session, static_cast<size_t>(*val));
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_alt_speed_time_day); val != nullptr)
    {
        tr_sessionSetAltSpeedDay(session, static_cast<tr_sched_day>(*val));
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_alt_speed_time_enabled); val != nullptr)
    {
        tr_sessionUseAltSpeedTime(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_blocklist_enabled); val != nullptr)
    {
        session->useBlocklist(*val);
    }

    if (auto const* val = args_in.find_if<std::string_view>(TR_KEY_blocklist_url); val != nullptr)
    {
        session->setBlocklistUrl(*val);
    }

    if (download_dir && !std::empty(*download_dir))
    {
        session->setDownloadDir(*download_dir);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_queue_stalled_minutes); val != nullptr)
    {
        tr_sessionSetQueueStalledMinutes(session, static_cast<int>(*val));
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_queue_stalled_enabled); val != nullptr)
    {
        tr_sessionSetQueueStalledEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<std::string_view>(TR_KEY_default_trackers); val != nullptr)
    {
        session->setDefaultTrackers(*val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_download_queue_size); val != nullptr)
    {
        tr_sessionSetQueueSize(session, TR_DOWN, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_download_queue_enabled); val != nullptr)
    {
        tr_sessionSetQueueEnabled(session, TR_DOWN, *val);
    }

    if (incomplete_dir && !std::empty(*incomplete_dir))
    {
        session->setIncompleteDir(*incomplete_dir);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_incomplete_dir_enabled); val != nullptr)
    {
        session->useIncompleteDir(*val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_peer_limit_global); val != nullptr)
    {
        tr_sessionSetPeerLimit(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_peer_limit_per_torrent); val != nullptr)
    {
        tr_sessionSetPeerLimitPerTorrent(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_pex_enabled); val != nullptr)
    {
        tr_sessionSetPexEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_dht_enabled); val != nullptr)
    {
        tr_sessionSetDHTEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_utp_enabled); val != nullptr)
    {
        tr_sessionSetUTPEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_lpd_enabled); val != nullptr)
    {
        tr_sessionSetLPDEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_peer_port_random_on_start); val != nullptr)
    {
        tr_sessionSetPeerPortRandomOnStart(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_peer_port); val != nullptr)
    {
        tr_sessionSetPeerPort(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_port_forwarding_enabled); val != nullptr)
    {
        tr_sessionSetPortForwardingEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_rename_partial_files); val != nullptr)
    {
        tr_sessionSetIncompleteFileNamingEnabled(session, *val);
    }

    if (auto const* val = args_in.find_if<double>(TR_KEY_seedRatioLimit); val != nullptr)
    {
        tr_sessionSetRatioLimit(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_seedRatioLimited); val != nullptr)
    {
        tr_sessionSetRatioLimited(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_idle_seeding_limit); val != nullptr)
    {
        tr_sessionSetIdleLimit(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_idle_seeding_limit_enabled); val != nullptr)
    {
        tr_sessionSetIdleLimited(session, *val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_start_added_torrents); val != nullptr)
    {
        tr_sessionSetPaused(session, !*val);
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_seed_queue_enabled); val != nullptr)
    {
        tr_sessionSetQueueEnabled(session, TR_UP, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_seed_queue_size); val != nullptr)
    {
        tr_sessionSetQueueSize(session, TR_UP, *val);
    }

    for (auto const& [enabled_key, script_key, script] : tr_session::Scripts)
    {
        if (auto const* val = args_in.find_if<bool>(enabled_key); val != nullptr)
        {
            session->useScript(script, *val);
        }

        if (auto const* val = args_in.find_if<std::string_view>(script_key); val != nullptr)
        {
            session->setScript(script, *val);
        }
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_trash_original_torrent_files); val != nullptr)
    {
        tr_sessionSetDeleteSource(session, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_speed_limit_down); val != nullptr)
    {
        tr_sessionSetSpeedLimit_KBps(session, TR_DOWN, static_cast<tr_kilobytes_per_second_t>(*val));
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_speed_limit_down_enabled); val != nullptr)
    {
        tr_sessionLimitSpeed(session, TR_DOWN, *val);
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_speed_limit_up); val != nullptr)
    {
        tr_sessionSetSpeedLimit_KBps(session, TR_UP, static_cast<tr_kilobytes_per_second_t>(*val));
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_speed_limit_up_enabled); val != nullptr)
    {
        tr_sessionLimitSpeed(session, TR_UP, *val);
    }

    if (auto const* val = args_in.find_if<std::string_view>(TR_KEY_encryption); val != nullptr)
    {
        if (*val == "required"sv)
        {
            tr_sessionSetEncryption(session, TR_ENCRYPTION_REQUIRED);
        }
        else if (*val == "tolerated"sv)
        {
            tr_sessionSetEncryption(session, TR_CLEAR_PREFERRED);
        }
        else
        {
            tr_sessionSetEncryption(session, TR_ENCRYPTION_PREFERRED);
        }
    }

    if (auto const* val = args_in.find_if<int64_t>(TR_KEY_anti_brute_force_threshold); val != nullptr)
    {
        tr_sessionSetAntiBruteForceThreshold(session, static_cast<int>(*val));
    }

    if (auto const* val = args_in.find_if<bool>(TR_KEY_anti_brute_force_enabled); val != nullptr)
    {
        tr_sessionSetAntiBruteForceEnabled(session, *val);
    }

    session->rpcNotify(TR_RPC_SESSION_CHANGED, nullptr);

    return nullptr;
}

char const* sessionStats(
    tr_session* session,
    tr_variant::Map const& /*args_in*/,
    tr_variant::Map& args_out,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const make_stats_map = [](auto const& stats)
    {
        auto stats_map = tr_variant::Map{ 5U };
        stats_map.try_emplace(TR_KEY_downloadedBytes, stats.downloadedBytes);
        stats_map.try_emplace(TR_KEY_filesAdded, stats.filesAdded);
        stats_map.try_emplace(TR_KEY_secondsActive, stats.secondsActive);
        stats_map.try_emplace(TR_KEY_sessionCount, stats.sessionCount);
        stats_map.try_emplace(TR_KEY_uploadedBytes, stats.uploadedBytes);
        return stats_map;
    };

    auto const& torrents = session->torrents();
    auto const total = std::size(torrents);
    auto const n_running = std::count_if(
        std::begin(torrents),
        std::end(torrents),
        [](auto const* tor) { return tor->is_running(); });

    args_out.reserve(std::size(args_out) + 7U);
    args_out.try_emplace(TR_KEY_activeTorrentCount, n_running);
    args_out.try_emplace(TR_KEY_cumulative_stats, make_stats_map(session->stats().cumulative()));
    args_out.try_emplace(TR_KEY_current_stats, make_stats_map(session->stats().current()));
    args_out.try_emplace(TR_KEY_downloadSpeed, session->pieceSpeedBps(TR_DOWN));
    args_out.try_emplace(TR_KEY_pausedTorrentCount, total - n_running);
    args_out.try_emplace(TR_KEY_torrentCount, total);
    args_out.try_emplace(TR_KEY_uploadSpeed, session->pieceSpeedBps(TR_UP));

    return nullptr;
}

[[nodiscard]] constexpr std::string_view getEncryptionModeString(tr_encryption_mode mode)
{
    switch (mode)
    {
    case TR_CLEAR_PREFERRED:
        return "tolerated"sv;

    case TR_ENCRYPTION_REQUIRED:
        return "required"sv;

    default:
        return "preferred"sv;
    }
}

[[nodiscard]] tr_variant make_session_field(tr_session const& session, tr_quark const key)
{
    // clang-format off
    switch (key)
    {
    case TR_KEY_alt_speed_down: return static_cast<int64_t>(tr_sessionGetAltSpeed_KBps(&session, TR_DOWN));
    case TR_KEY_alt_speed_enabled: return tr_sessionUsesAltSpeed(&session);
    case TR_KEY_alt_speed_time_begin: return tr_sessionGetAltSpeedBegin(&session);
    case TR_KEY_alt_speed_time_day: return static_cast<int64_t>(tr_sessionGetAltSpeedDay(&session));
    case TR_KEY_alt_speed_time_enabled: return tr_sessionUsesAltSpeedTime(&session);
    case TR_KEY_alt_speed_time_end: return tr_sessionGetAltSpeedEnd(&session);
    case TR_KEY_alt_speed_up: return static_cast<int64_t>(tr_sessionGetAltSpeed_KBps(&session, TR_UP));
    case TR_KEY_anti_brute_force_enabled: return tr_sessionGetAntiBruteForceEnabled(&session);
    case TR_KEY_anti_brute_force_threshold: return tr_sessionGetAntiBruteForceThreshold(&session);
    case TR_KEY_blocklist_enabled: return session.useBlocklist();
    case TR_KEY_blocklist_size: return tr_blocklistGetRuleCount(&session);
    case TR_KEY_blocklist_url: return session.blocklistUrl();
    case TR_KEY_cache_size_mb: return tr_sessionGetCacheLimit_MB(&session);
    case TR_KEY_config_dir: return session.configDir();
    case TR_KEY_default_trackers: return session.defaultTrackersStr();
    case TR_KEY_dht_enabled: return session.allowsDHT();
    case TR_KEY_download_dir: return session.downloadDir();
    case TR_KEY_download_dir_free_space: return tr_sys_path_get_capacity(session.downloadDir()).value_or(tr_sys_path_capacity{}).free;
    case TR_KEY_download_queue_enabled: return session.queueEnabled(TR_DOWN);
    case TR_KEY_download_queue_size: return session.queueSize(TR_DOWN);
    case TR_KEY_encryption: return getEncryptionModeString(tr_sessionGetEncryption(&session));
    case TR_KEY_idle_seeding_limit: return session.idleLimitMinutes();
    case TR_KEY_idle_seeding_limit_enabled: return session.isIdleLimited();
    case TR_KEY_incomplete_dir: return session.incompleteDir();
    case TR_KEY_incomplete_dir_enabled: return session.useIncompleteDir();
    case TR_KEY_lpd_enabled: return session.allowsLPD();
    case TR_KEY_peer_limit_global: return session.peerLimit();
    case TR_KEY_peer_limit_per_torrent: return session.peerLimitPerTorrent();
    case TR_KEY_peer_port: return session.advertisedPeerPort().host();
    case TR_KEY_peer_port_random_on_start: return session.isPortRandom();
    case TR_KEY_pex_enabled: return session.allows_pex();
    case TR_KEY_port_forwarding_enabled: return tr_sessionIsPortForwardingEnabled(&session);
    case TR_KEY_queue_stalled_enabled: return session.queueStalledEnabled();
    case TR_KEY_queue_stalled_minutes: return session.queueStalledMinutes();
    case TR_KEY_rename_partial_files: return session.isIncompleteFileNamingEnabled();
    case TR_KEY_rpc_version: return RpcVersion;
    case TR_KEY_rpc_version_minimum: return RpcVersionMin;
    case TR_KEY_rpc_version_semver: return RpcVersionSemver;
    case TR_KEY_script_torrent_added_enabled: return session.useScript(TR_SCRIPT_ON_TORRENT_ADDED);
    case TR_KEY_script_torrent_added_filename: return session.script(TR_SCRIPT_ON_TORRENT_ADDED);
    case TR_KEY_script_torrent_done_enabled: return session.useScript(TR_SCRIPT_ON_TORRENT_DONE);
    case TR_KEY_script_torrent_done_filename: return session.script(TR_SCRIPT_ON_TORRENT_DONE);
    case TR_KEY_script_torrent_done_seeding_enabled: return session.useScript(TR_SCRIPT_ON_TORRENT_DONE_SEEDING);
    case TR_KEY_script_torrent_done_seeding_filename: return session.script(TR_SCRIPT_ON_TORRENT_DONE_SEEDING);
    case TR_KEY_seedRatioLimit: return session.desiredRatio();
    case TR_KEY_seedRatioLimited: return session.isRatioLimited();
    case TR_KEY_seed_queue_enabled: return session.queueEnabled(TR_UP);
    case TR_KEY_seed_queue_size: return session.queueSize(TR_UP);
    case TR_KEY_session_id: return session.sessionId();
    case TR_KEY_speed_limit_down: return static_cast<int64_t>(tr_sessionGetSpeedLimit_KBps(&session, TR_DOWN));
    case TR_KEY_speed_limit_down_enabled: return session.isSpeedLimited(TR_DOWN);
    case TR_KEY_speed_limit_up: return static_cast<int64_t>(tr_sessionGetSpeedLimit_KBps(&session, TR_UP));
    case TR_KEY_speed_limit_up_enabled: return session.isSpeedLimited(TR_UP);
    case TR_KEY_start_added_torrents: return !session.shouldPauseAddedTorrents();
    case TR_KEY_tcp_enabled: return session.allowsTCP();
    case TR_KEY_trash_original_torrent_files: return session.shouldDeleteSource();
    case TR_KEY_units: return tr_formatter_get_units();
    case TR_KEY_utp_enabled: return session.allowsUTP();
    case TR_KEY_version: return LONG_VERSION_STRING;
    default: return tr_variant{};
    }
    // clang-format on
}

namespace session_get_helpers
{
[[nodiscard]] auto get_session_fields(tr_variant::Vector const* fields_vec)
{
    auto fields = std::set<tr_quark>{};

    if (fields_vec != nullptr)
    {
        for (auto const& field_var : *fields_vec)
        {
            if (auto const* field_name = field_var.get_if<std::string_view>(); field_name != nullptr)
            {
                if (auto const field_id = tr_quark_lookup(*field_name); field_id)
                {
                    fields.insert(*field_id);
                }
            }
        }
    }

    if (std::empty(fields)) // no fields specified; get them all
    {
        for (tr_quark field_id = TR_KEY_NONE + 1; field_id < TR_N_KEYS; ++field_id)
        {
            fields.insert(field_id);
        }
    }

    return fields;
}
} // namespace session_get_helpers

char const* sessionGet(
    tr_session* session,
    tr_variant::Map const& args_in,
    tr_variant::Map& args_out,
    tr_rpc_idle_data* /*idle_data*/)
{
    using namespace session_get_helpers;

    for (auto const key : get_session_fields(args_in.find_if<tr_variant::Vector>(TR_KEY_fields)))
    {
        if (auto var = make_session_field(*session, key); var.has_value())
        {
            args_out.try_emplace(key, std::move(var));
        }
    }

    return nullptr;
}

char const* freeSpace(
    tr_session* /*session*/,
    tr_variant::Map const& args_in,
    tr_variant::Map& args_out,
    tr_rpc_idle_data* /*idle_data*/)
{
    auto const path = args_in.value_if<std::string_view>(TR_KEY_path);
    if (!path)
    {
        return "directory path argument is missing";
    }

    if (tr_sys_path_is_relative(*path))
    {
        return "directory path is not absolute";
    }

    /* get the free space */
    auto const old_errno = errno;
    tr_error* error = nullptr;
    auto const capacity = tr_sys_path_get_capacity(*path, &error);
    char const* const err = error != nullptr ? tr_strerror(error->code) : nullptr;
    tr_error_clear(&error);
    errno = old_errno;

    /* response */
    args_out.try_emplace(TR_KEY_path, *path);
    args_out.try_emplace(TR_KEY_size_bytes, capacity ? capacity->free : -1);
    args_out.try_emplace(TR_KEY_total_size, capacity ? capacity->total : -1);
    return err;
}

// ---

char const* sessionClose(
    tr_session* session,
    tr_variant::Map const& /*args_in*/,
    tr_variant::Map& /*args_out*/,
    tr_rpc_idle_data* /*idle_data*/)
{
    session->rpcNotify(TR_RPC_SESSION_CLOSE, nullptr);
    return nullptr;
}

// ---

using handler = char const* (*)(tr_session*, tr_variant::Map const&, tr_variant::Map&, struct tr_rpc_idle_data*);

struct rpc_method
{
    std::string_view name;
    bool immediate;
    handler func;
};

auto constexpr Methods = std::array<rpc_method, 24>{ {
    { "blocklist-update"sv, false, blocklistUpdate },
    { "free-space"sv, true, freeSpace },
    { "group-get"sv, true, groupGet },
    { "group-set"sv, true, groupSet },
    { "port-test"sv, false, portTest },
    { "queue-move-bottom"sv, true, queueMoveBottom },
    { "queue-move-down"sv, true, queueMoveDown },
    { "queue-move-top"sv, true, queueMoveTop },
    { "queue-move-up"sv, true, queueMoveUp },
    { "session-close"sv, true, sessionClose },
    { "session-get"sv, true, sessionGet },
    { "session-set"sv, true, sessionSet },
    { "session-stats"sv, true, sessionStats },
    { "torrent-add"sv, false, torrentAdd },
    { "torrent-get"sv, true, torrentGet },
    { "torrent-reannounce"sv, true, torrentReannounce },
    { "torrent-remove"sv, true, torrentRemove },
    { "torrent-rename-path"sv, false, torrentRenamePath },
    { "torrent-set"sv, true, torrentSet },
    { "torrent-set-location"sv, true, torrentSetLocation },
    { "torrent-start"sv, true, torrentStart },
    { "torrent-start-now"sv, true, torrentStartNow },
    { "torrent-stop"sv, true, torrentStop },
    { "torrent-verify"sv, true, torrentVerify },
} };

void noop_response_callback(tr_session* /*session*/, tr_variant* /*response*/, void* /*user_data*/)
{
}

} // namespace

void tr_rpc_request_exec_json(
    tr_session* session,
    tr_variant const* request,
    tr_rpc_response_func callback,
    void* callback_user_data)
{
    auto const lock = session->unique_lock();

    auto const* const request_map = request->get_if<tr_variant::Map>();

    if (callback == nullptr)
    {
        callback = noop_response_callback;
    }

    // find the args
    auto const empty_args = tr_variant::Map{};
    auto const* args_in = &empty_args;
    if (request_map != nullptr)
    {
        if (auto const* val = request_map->find_if<tr_variant::Map>(TR_KEY_arguments); val != nullptr)
        {
            args_in = val;
        }
    }

    auto const tag = args_in->value_if<int64_t>(TR_KEY_tag);

    // find the requested method
    rpc_method const* method = nullptr;
    if (request_map != nullptr)
    {
        if (auto const* val = request_map->find_if<std::string_view>(TR_KEY_method); val != nullptr)
        {
            if (auto const iter = std::find_if(
                    std::begin(Methods),
                    std::end(Methods),
                    [val](auto const& row) { return row.name == *val; });
                iter != std::end(Methods))
            {
                method = &*iter;
            }
        }
    }

    if (method == nullptr) // return an error if we couldn't figure out what to do
    {
        auto response = tr_variant::Map{ 3U };
        response.try_emplace(TR_KEY_arguments, 0);
        response.try_emplace(TR_KEY_result, "no method name");
        if (tag.has_value())
        {
            response.try_emplace(TR_KEY_tag, *tag);
        }

        auto tmp = tr_variant{ std::move(response) };
        (*callback)(session, &tmp, callback_user_data);
        return;
    }

    if (method->immediate) // if it's an immediate action, do it now
    {
        auto args_out = tr_variant::Map{};
        char const* const result = (*method->func)(session, *args_in, args_out, nullptr);

        auto response = tr_variant::Map{ 3U };
        response.try_emplace(TR_KEY_arguments, std::move(args_out));
        response.try_emplace(TR_KEY_result, result != nullptr ? result : "success");
        if (tag.has_value())
        {
            response.try_emplace(TR_KEY_tag, *tag);
        }

        auto tmp = tr_variant{ std::move(response) };
        (*callback)(session, &tmp, callback_user_data);
        return;
    }

    // async method

    auto* const data = new tr_rpc_idle_data{};
    data->session = session;
    data->tag = tag;
    data->callback = callback;
    data->callback_user_data = callback_user_data;
    if (char const* const errmsg = (*method->func)(session, *args_in, data->args_out, data); errmsg != nullptr)
    {
        // Async operation failed prematurely? Invoke callback to ensure client gets a reply
        tr_idle_function_done(data, errmsg);
    }
}

/**
 * Munge the URI into a usable form.
 *
 * We have very loose typing on this to make the URIs as simple as possible:
 * - anything not a 'tag' or 'method' is automatically in 'arguments'
 * - values that are all-digits are numbers
 * - values that are all-digits or commas are number lists
 * - all other values are strings
 */
void tr_rpc_parse_list_str(tr_variant* setme, std::string_view str)
{
    auto const values = tr_num_parse_range(str);
    auto const value_count = std::size(values);

    if (value_count == 0)
    {
        *setme = str;
    }
    else if (value_count == 1)
    {
        *setme = values[0];
    }
    else
    {
        auto vec = tr_variant::Vector{};
        vec.reserve(value_count);
        std::copy(std::begin(values), std::end(values), std::back_inserter(vec));
        *setme = std::move(vec);
    }
}
