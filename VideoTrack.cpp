#include <sstream>
#include <algorithm>
#include "VideoTrack.h"

using namespace std;

namespace DataLayer
{
    std::function<bool(const VideoClipHolder&, const VideoClipHolder&)> VideoTrack::CLIP_SORT_CMP =
        [](const VideoClipHolder& a, const VideoClipHolder& b)
        {
            return a->Start() < b->Start();
        };

    std::function<bool(const VideoOverlapHolder&, const VideoOverlapHolder&)> VideoTrack::OVERLAP_SORT_CMP =
        [](const VideoOverlapHolder& a, const VideoOverlapHolder& b)
        {
            return a->Start() < b->Start();
        };

    VideoTrack::VideoTrack(int64_t id, uint32_t outWidth, uint32_t outHeight, const MediaInfo::Ratio& frameRate)
        : m_id(id), m_outWidth(outWidth), m_outHeight(outHeight), m_frameRate(frameRate)
    {
        m_readClipIter = m_clips.begin();
    }

    // uint32_t VideoTrack::AddNewClip(const std::string& url, double start, double startOffset, double endOffset)
    // {
    //     lock_guard<recursive_mutex> lk(m_apiLock);

    //     MediaParserHolder hParser = CreateMediaParser();
    //     if (!hParser->Open(url))
    //         throw runtime_error(hParser->GetError());

    //     return AddNewClip(hParser, start, startOffset, endOffset);
    // }

    // uint32_t VideoTrack::AddNewClip(MediaParserHolder hParser, double start, double startOffset, double endOffset)
    // {
    //     lock_guard<recursive_mutex> lk(m_apiLock);

    //     VideoClipHolder hClip(new VideoClip(hParser, m_outWidth, m_outHeight, m_frameRate, start, startOffset, endOffset));
    //     const uint32_t clipId = hClip->Id();
    //     InsertClip(hClip, start);
    //     SeekTo((double)m_readFrames*m_frameRate.den/m_frameRate.num);

    //     return clipId;
    // }

    void VideoTrack::InsertClip(VideoClipHolder hClip)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!CheckClipRangeValid(hClip->Id(), hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for inserting clip!");

        // add this clip into clip list
        hClip->SetDirection(m_readForward);
        m_clips.push_back(hClip);
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        VideoClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
        // update seek state
        SeekTo((double)m_readFrames*m_frameRate.den/m_frameRate.num);
    }

    void VideoTrack::MoveClip(int64_t id, double start)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        VideoClipHolder hClip = GetClipById(id);
        if (!hClip)
            throw invalid_argument("Invalid value for argument 'id'!");

        if (hClip->Start() == start)
            return;
        else
            hClip->SetTimeLineOffset(start);

        if (!CheckClipRangeValid(id, hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for moving clip!");

        // update clip order
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        VideoClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
        // update seek state
        SeekTo((double)m_readFrames*m_frameRate.den/m_frameRate.num);
    }

    void VideoTrack::ChangeClipRange(int64_t id, double startOffset, double endOffset)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        VideoClipHolder hClip = GetClipById(id);
        if (!hClip)
            throw invalid_argument("Invalid value for argument 'id'!");

        bool rangeChanged = false;
        if (startOffset != hClip->StartOffset())
        {
            hClip->ChangeStartOffset(startOffset);
            rangeChanged = true;
        }
        if (endOffset != hClip->EndOffset())
        {
            hClip->ChangeEndOffset(endOffset);
            rangeChanged = true;
        }
        if (!rangeChanged)
            return;

        if (!CheckClipRangeValid(id, hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for changing clip range!");

        // update clip order
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        VideoClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
        // update seek state
        SeekTo((double)m_readFrames*m_frameRate.den/m_frameRate.num);
    }

    VideoClipHolder VideoTrack::RemoveClipById(int64_t clipId)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        auto iter = find_if(m_clips.begin(), m_clips.end(), [clipId](const VideoClipHolder& clip) {
            return clip->Id() == clipId;
        });
        if (iter == m_clips.end())
            return nullptr;

        VideoClipHolder hClip = (*iter);
        m_clips.erase(iter);
        const double clipStart = hClip->Start();
        const double clipEnd = clipStart+hClip->Duration();
        double readPos = (double)m_readFrames*m_frameRate.den/m_frameRate.num;
        if (readPos >= clipStart && readPos < clipEnd)
            SeekTo(readPos);

        if (m_clips.empty())
            m_duration = 0;
        else
        {
            VideoClipHolder lastClip = m_clips.back();
            m_duration = lastClip->Start()+lastClip->Duration();
        }
        return hClip;
    }

    VideoClipHolder VideoTrack::RemoveClipByIndex(uint32_t index)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);

        if (index >= m_clips.size())
            throw invalid_argument("Argument 'index' exceeds the count of clips!");

        auto iter = m_clips.begin();
        while (index > 0)
        {
            iter++; index--;
        }

        VideoClipHolder hClip = (*iter);
        m_clips.erase(iter);
        const double clipStart = hClip->Start();
        const double clipEnd = clipStart+hClip->Duration();
        double readPos = (double)m_readFrames*m_frameRate.den/m_frameRate.num;
        if (readPos >= clipStart && readPos < clipEnd)
            SeekTo(readPos);

        if (m_clips.empty())
            m_duration = 0;
        else
        {
            VideoClipHolder lastClip = m_clips.back();
            m_duration = lastClip->Start()+lastClip->Duration();
        }
        return hClip;
    }

    void VideoTrack::SeekTo(double pos)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (pos < 0)
            throw invalid_argument("Argument 'pos' can NOT be NEGATIVE!");

        if (m_readForward)
        {
            // update read clip iterator
            m_readClipIter = m_clips.end();
            {
                auto iter = m_clips.begin();
                while (iter != m_clips.end())
                {
                    const VideoClipHolder& hClip = *iter;
                    double clipPos = pos-hClip->Start();
                    hClip->SeekTo(clipPos);
                    if (m_readClipIter == m_clips.end() && clipPos < hClip->Duration())
                        m_readClipIter = iter;
                    iter++;
                }
            }
            // update read overlap iterator
            m_readOverlapIter = m_overlaps.end();
            {
                auto iter = m_overlaps.begin();
                while (iter != m_overlaps.end())
                {
                    const VideoOverlapHolder& hOverlap = *iter;
                    double overlapPos = pos-hOverlap->Start();
                    if (m_readOverlapIter == m_overlaps.end() && overlapPos < hOverlap->Duration())
                    {
                        m_readOverlapIter = iter;
                        break;
                    }
                    iter++;
                }
            }
        }
        else
        {
            m_readClipIter = m_clips.end();
            {
                auto riter = m_clips.rbegin();
                while (riter != m_clips.rend())
                {
                    const VideoClipHolder& hClip = *riter;
                    double clipPos = pos-hClip->Start();
                    hClip->SeekTo(clipPos);
                    if (m_readClipIter == m_clips.end() && clipPos >= 0)
                        m_readClipIter = riter.base();
                    riter++;
                }
            }
            m_readOverlapIter = m_overlaps.end();
            {
                auto riter = m_overlaps.rbegin();
                while (riter != m_overlaps.rend())
                {
                    const VideoOverlapHolder& hOverlap = *riter;
                    double overlapPos = pos-hOverlap->Start();
                    if (m_readOverlapIter == m_overlaps.end() && overlapPos >= 0)
                        m_readOverlapIter = riter.base();
                    riter++;
                }
            }
        }

        m_readFrames = (int64_t)(pos*m_frameRate.num/m_frameRate.den);
    }

    void VideoTrack::ReadVideoFrame(ImGui::ImMat& vmat)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        vmat.release();
        double readPos = (double)m_readFrames*m_frameRate.den/m_frameRate.num;

        if (m_readForward)
        {
            // first, find the image from a overlap
            while (m_readOverlapIter != m_overlaps.end() && readPos >= (*m_readOverlapIter)->Start())
            {
                auto& hOverlap = *m_readOverlapIter;
                bool eof = false;
                if (readPos < hOverlap->End())
                {
                    hOverlap->ReadVideoFrame(readPos-hOverlap->Start(), vmat, eof);
                    break;
                }
                else
                    m_readOverlapIter++;
            }

            if (vmat.empty())
            {
                // then try to read the image from a clip
                while (m_readClipIter != m_clips.end() && readPos >= (*m_readClipIter)->Start())
                {
                    auto& hClip = *m_readClipIter;
                    bool eof = false;
                    if (readPos < hClip->End())
                    {
                        hClip->ReadVideoFrame(readPos-hClip->Start(), vmat, eof);
                        break;
                    }
                    else
                        m_readClipIter++;
                }
            }

            vmat.time_stamp = readPos;
            m_readFrames++;
        }
        else
        {
            while (m_readOverlapIter != m_overlaps.begin() && (m_readOverlapIter == m_overlaps.end() || readPos < (*m_readOverlapIter)->Start()))
                m_readOverlapIter--;
            if (m_readOverlapIter != m_overlaps.end())
            {
                auto& hOverlap = *m_readOverlapIter;
                bool eof = false;
                if (readPos >= hOverlap->Start() && readPos < hOverlap->End())
                    hOverlap->ReadVideoFrame(readPos-hOverlap->Start(), vmat, eof);
            }

            if (vmat.empty())
            {
                while (m_readClipIter != m_clips.begin() && (m_readClipIter == m_clips.end() || readPos < (*m_readClipIter)->Start()))
                    m_readClipIter--;
                if (m_readClipIter != m_clips.end())
                {
                    auto& hClip = *m_readClipIter;
                    bool eof = false;
                    if (readPos < hClip->End())
                        hClip->ReadVideoFrame(readPos-hClip->Start(), vmat, eof);
                }
            }

            vmat.time_stamp = readPos;
            m_readFrames--;
        }
    }

    void VideoTrack::SetDirection(bool forward)
    {
        if (m_readForward == forward)
            return;
        m_readForward = forward;
        for (auto& clip : m_clips)
            clip->SetDirection(forward);
        // double readPos = (double)m_readFrames*m_frameRate.den/m_frameRate.num;
        // SeekTo(readPos);
    }

    VideoClipHolder VideoTrack::GetClipByIndex(uint32_t index)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (index >= m_clips.size())
            return nullptr;
        auto iter = m_clips.begin();
        while (index > 0)
        {
            iter++; index--;
        }
        return *iter;
    }

    VideoClipHolder VideoTrack::GetClipById(int64_t id)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        auto iter = find_if(m_clips.begin(), m_clips.end(), [id] (const VideoClipHolder& clip) {
            return clip->Id() == id;
        });
        if (iter != m_clips.end())
            return *iter;
        return nullptr;
    }

    bool VideoTrack::CheckClipRangeValid(int64_t clipId, double start, double end)
    {
        for (auto& overlap : m_overlaps)
        {
            if (clipId == overlap->FrontClip()->Id() || clipId == overlap->RearClip()->Id())
                continue;
            if (start > overlap->Start() && start < overlap->End() ||
                end > overlap->Start() && end < overlap->End())
                return false;
        }
        return true;
    }

    void VideoTrack::UpdateClipOverlap(VideoClipHolder hUpdateClip)
    {
        const int64_t id1 = hUpdateClip->Id();
        // remove invalid overlaps
        auto ovIter = m_overlaps.begin();
        while (ovIter != m_overlaps.end())
        {
            auto& hOverlap = *ovIter;
            if (hOverlap->FrontClip()->Id() == id1 || hOverlap->RearClip()->Id() == id1)
            {
                hOverlap->Update();
                if (hOverlap->Duration() <= 0)
                {
                    ovIter = m_overlaps.erase(ovIter);
                    continue;
                }
            }
            ovIter++;
        }
        // add new overlaps
        for (auto& clip : m_clips)
        {
            if (hUpdateClip == clip)
                continue;
            if (VideoOverlap::HasOverlap(hUpdateClip, clip))
            {
                const int64_t id2 = clip->Id();
                auto iter = find_if(m_overlaps.begin(), m_overlaps.end(), [id1, id2] (const VideoOverlapHolder& overlap) {
                    const int64_t idf = overlap->FrontClip()->Id();
                    const int64_t idr = overlap->RearClip()->Id();
                    return id1 == idf && id2 == idr || id1 == idr && id2 == idf;
                });
                if (iter == m_overlaps.end())
                {
                    VideoOverlapHolder hOverlap(new VideoOverlap(0, hUpdateClip, clip));
                    m_overlaps.push_back(hOverlap);
                }
            }
        }

        // sort overlap by 'Start' time
        m_overlaps.sort(OVERLAP_SORT_CMP);
    }
}