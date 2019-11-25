/*
 * Project: curve
 * Created Date: Mon Dec 24 2018
 * Author: xuchaojie
 * Copyright (c) 2018 netease
 */

#include "src/snapshotcloneserver/snapshot/snapshot_core.h"

#include <glog/logging.h>
#include <utility>
#include <algorithm>

#include "src/snapshotcloneserver/common/define.h"
#include "src/snapshotcloneserver/snapshot/snapshot_task.h"

#include "src/common/uuid.h"

using ::curve::common::UUIDGenerator;
using ::curve::common::NameLockGuard;
using ::curve::common::LockGuard;

namespace curve {
namespace snapshotcloneserver {

int SnapshotCoreImpl::Init() {
    int ret = threadPool_->Start();
    if (ret < 0) {
        LOG(ERROR) << "SnapshotCoreImpl, thread start fail, ret = " << ret;
        return ret;
    }
    return kErrCodeSuccess;
}

int SnapshotCoreImpl::CreateSnapshotPre(const std::string &file,
    const std::string &user,
    const std::string &snapshotName,
    SnapshotInfo *snapInfo) {
    NameLockGuard lockGuard(snapshotNameLock_, file);
    std::vector<SnapshotInfo> fileInfo;
    int ret = metaStore_->GetSnapshotList(file, &fileInfo);
    for (auto& snap : fileInfo) {
        if (Status::error == snap.GetStatus()) {
            LOG(INFO) << "Can not create snapshot when snapshot has error,"
                      << " error snapshot id = " << snap.GetUuid();
            return kErrCodeSnapshotCannotCreateWhenError;
        }
    }
    if (fileInfo.size() >= maxSnapshotLimit_) {
        LOG(ERROR) << "Snapshot count reach the max limit.";
        return kErrCodeSnapshotCountReachLimit;
    }

    FInfo fInfo;
    ret = client_->GetFileInfo(file, user, &fInfo);
    switch (ret) {
        case LIBCURVE_ERROR::OK:
            break;
        case -LIBCURVE_ERROR::NOTEXIST:
            LOG(ERROR) << "create snapshot file not exist"
                       << ", file = " << file
                       << ", user = " << user
                       << ", snapshotName = " << snapshotName;
            return kErrCodeFileNotExist;
        case -LIBCURVE_ERROR::AUTHFAIL:
            LOG(ERROR) << "create snapshot by invalid user"
                       << ", file = " << file
                       << ", user = " << user
                       << ", snapshotName = " << snapshotName;
            return kErrCodeInvalidUser;
        default:
            LOG(ERROR) << "GetFileInfo encounter an error"
                       << ", ret = " << ret
                       << ", file = " << file
                       << ", user = " << user;
            return kErrCodeInternalError;
    }

    if (fInfo.filestatus != FileStatus::Created &&
        fInfo.filestatus != FileStatus::Cloned) {
        LOG(ERROR) << "Can not create snapshot when file status = "
                   << static_cast<int>(fInfo.filestatus);
        return kErrCodeFileStatusInvalid;
    }

    UUID uuid = UUIDGenerator().GenerateUUID();
    SnapshotInfo info(uuid, user, file, snapshotName);
    info.SetStatus(Status::pending);
    ret = metaStore_->AddSnapshot(info);
    if (ret < 0) {
        LOG(ERROR) << "AddSnapshot error,"
                   << " ret = " << ret
                   << ", uuid = " << uuid
                   << ", fileName = " << file
                   << ", snapshotName = " << snapshotName;
        return ret;
    }
    *snapInfo = info;
    return kErrCodeSuccess;
}

constexpr uint32_t kProgressCreateSnapshotOnCurvefsComplete = 5;
constexpr uint32_t kProgressBuildChunkIndexDataComplete = 6;
constexpr uint32_t kProgressBuildSnapshotMapComplete = 10;
constexpr uint32_t kProgressTransferSnapshotDataStart =
                       kProgressBuildSnapshotMapComplete;
constexpr uint32_t kProgressTransferSnapshotDataComplete = 99;
constexpr uint32_t kProgressComplete = 100;

/**
 * @brief 异步执行创建快照任务并更新任务进度
 *
 * 快照进度规划如下:
 *
 *  |CreateSnapshotOnCurvefs| BuildChunkIndexData | BuildSnapshotMap | TransferSnapshotData | UpdateSnapshot | //NOLINT
 *  | 5%                    | 6%                  | 10%              | 10%~99%              | 100%           | //NOLINT
 *
 *
 *  异步执行期间发生error与cancel情况说明：
 *  1. 发生error将导致整个异步任务直接中断，并且不做任何清理动作:
 *  发生error时，一般系统存在异常，清理动作很可能不能完成，
 *  因此，不进行任何清理，只置状态，待人工干预排除异常之后，
 *  使用DeleteSnapshot功能去手动删除error状态的快照。
 *  2. 发生cancel时则以创建功能相反的顺序依次进行清理动作，
 *  若清理过程发生error,则立即中断，之后同error过程。
 *
 * @param task 快照任务
 */
void SnapshotCoreImpl::HandleCreateSnapshotTask(
    std::shared_ptr<SnapshotTaskInfo> task) {
    int ret = kErrCodeSuccess;
    SnapshotInfo *info = &(task->GetSnapshotInfo());
    UUID uuid = task->GetUuid();
    uint64_t seqNum = info->GetSeqNum();
    std::string fileName = task->GetFileName();
    bool existIndexData = false;
    if (kUnInitializeSeqNum ==  seqNum) {
        ret = CreateSnapshotOnCurvefs(fileName, info, task);
        if (ret < 0) {
            LOG(ERROR) << "CreateSnapshotOnCurvefs error, "
                       << " ret = " << ret
                       << ", fileName = " << fileName;
            HandleCreateSnapshotError(task);
            return;
        }
        seqNum = info->GetSeqNum();
        existIndexData = false;
    } else {
        ChunkIndexDataName name(fileName, seqNum);
        // judge Is Exist indexData
        existIndexData = dataStore_->ChunkIndexDataExist(name);
    }

    task->SetProgress(kProgressCreateSnapshotOnCurvefsComplete);
    task->UpdateMetric();
    if (task->IsCanceled()) {
        CancelAfterCreateSnapshotOnCurvefs(task);
        return;
    }

    ChunkIndexData indexData;
    ChunkIndexDataName name(fileName, seqNum);
    // the key is segment index
    std::map<uint64_t, SegmentInfo> segInfos;
    if (existIndexData) {
        ret = dataStore_->GetChunkIndexData(name, &indexData);
        if (ret < 0) {
            LOG(ERROR) << "GetChunkIndexData error, "
                       << " ret = " << ret
                       << ", fileName = " << fileName
                       << ", seqNum = " << seqNum;
            HandleCreateSnapshotError(task);
            return;
        }

        task->SetProgress(kProgressBuildChunkIndexDataComplete);
        task->UpdateMetric();

        ret = BuildSegmentInfo(*info, &segInfos);
        if (ret < 0) {
            LOG(ERROR) << "BuildSegmentInfo error,"
                       << " ret = " << ret;
            HandleCreateSnapshotError(task);
            return;
        }
    } else {
        ret = BuildChunkIndexData(*info, &indexData, &segInfos, task);
        if (ret < 0) {
            LOG(ERROR) << "BuildChunkIndexData error, "
                       << " ret = " << ret;
            HandleCreateSnapshotError(task);
            return;
        }

        ret = dataStore_->PutChunkIndexData(name, indexData);
        if (ret < 0) {
            LOG(ERROR) << "PutChunkIndexData error, "
                       << " ret = " << ret;
            HandleCreateSnapshotError(task);
            return;
        }

        task->SetProgress(kProgressBuildChunkIndexDataComplete);
        task->UpdateMetric();
    }

    if (task->IsCanceled()) {
        CancelAfterCreateChunkIndexData(task);
        return;
    }

    FileSnapMap fileSnapshotMap;
    ret = BuildSnapshotMap(fileName,
        seqNum,
        &fileSnapshotMap);
    if (ret < 0) {
        LOG(ERROR) << "BuildSnapshotMap error, "
                   << " fileName = " << task->GetFileName()
                   << ", seqNum = " << seqNum;
        HandleCreateSnapshotError(task);
        return;
    }
    task->SetProgress(kProgressBuildSnapshotMapComplete);
    task->UpdateMetric();

    ret = TransferSnapshotData(indexData,
        *info,
        segInfos,
        [&fileSnapshotMap] (const ChunkDataName &chunkDataName) {
            return fileSnapshotMap.IsExistChunk(chunkDataName);
        },
        task);
    if (ret < 0) {
        LOG(ERROR) << "TransferSnapshotData error, "
                   << " ret = " << ret;
        HandleCreateSnapshotError(task);
        return;
    }
    task->SetProgress(kProgressTransferSnapshotDataComplete);
    task->UpdateMetric();

    LockGuard lockGuard(task->GetLockRef());
    if (task->IsCanceled()) {
        CancelAfterTransferSnapshotData(task, indexData, fileSnapshotMap);
        return;
    }

    info->SetStatus(Status::done);
    ret = metaStore_->UpdateSnapshot(*info);
    if (ret < 0) {
        LOG(ERROR) << "UpdateSnapshot error, "
                   << " ret = " << ret;
        HandleCreateSnapshotError(task);
        return;
    }
    task->SetProgress(kProgressComplete);

    task->Finish();
    LOG(INFO) << "CreateSnapshot Success.";
    return;
}

void SnapshotCoreImpl::CancelAfterTransferSnapshotData(
    std::shared_ptr<SnapshotTaskInfo> task,
    const ChunkIndexData &indexData,
    const FileSnapMap &fileSnapshotMap) {
    LOG(INFO) << "Cancel After TransferSnapshotData";
    std::vector<ChunkIndexType> chunkIndexVec = indexData.GetAllChunkIndex();
    for (auto &chunkIndex : chunkIndexVec) {
        ChunkDataName chunkDataName;
        indexData.GetChunkDataName(chunkIndex, &chunkDataName);
        if ((!fileSnapshotMap.IsExistChunk(chunkDataName)) &&
            (dataStore_->ChunkDataExist(chunkDataName))) {
            int ret =  dataStore_->DeleteChunkData(chunkDataName);
            if (ret < 0) {
                LOG(ERROR) << "DeleteChunkData error"
                           << "while canceling CreateSnapshot, "
                           << " ret = " << ret
                           << ", fileName = " << task->GetFileName()
                           << ", seqNum = " << chunkDataName.chunkSeqNum_
                           << ", chunkIndex = " << chunkDataName.chunkIndex_;
                HandleCreateSnapshotError(task);
                return;
            }
        }
    }
    CancelAfterCreateChunkIndexData(task);
}

void SnapshotCoreImpl::CancelAfterCreateChunkIndexData(
    std::shared_ptr<SnapshotTaskInfo> task) {
    LOG(INFO) << "Cancel After CreateChunkIndexData";
    SnapshotInfo &info = task->GetSnapshotInfo();
    UUID uuid = task->GetUuid();
    uint64_t seqNum = info.GetSeqNum();
    ChunkIndexDataName name(task->GetFileName(),
        seqNum);
    int ret = dataStore_->DeleteChunkIndexData(name);
    if (ret < 0) {
        LOG(ERROR) << "DeleteChunkIndexData error "
                   << "while canceling CreateSnapshot, "
                   << " ret = " << ret
                   << ", fileName = " << task->GetFileName()
                   << ", seqNum = " << seqNum;
        HandleCreateSnapshotError(task);
        return;
    }
    CancelAfterCreateSnapshotOnCurvefs(task);
}

void SnapshotCoreImpl::CancelAfterCreateSnapshotOnCurvefs(
    std::shared_ptr<SnapshotTaskInfo> task) {
    LOG(INFO) << "Cancel After CreateSnapshotOnCurvefs";
    SnapshotInfo &info = task->GetSnapshotInfo();
    UUID uuid = task->GetUuid();

    int ret = DeleteSnapshotOnCurvefs(info);
    if (ret < 0) {
        LOG(ERROR) << "DeleteSnapshotOnCurvefs fail.";
        HandleCreateSnapshotError(task);
        return;
    }
    HandleClearSnapshotOnMateStore(task);
}

void SnapshotCoreImpl::HandleClearSnapshotOnMateStore(
    std::shared_ptr<SnapshotTaskInfo> task) {
    int ret = metaStore_->DeleteSnapshot(task->GetUuid());
    if (ret < 0) {
        LOG(ERROR) << "MetaStore DeleteSnapshot error "
                   << "while cancel CreateSnapshot, "
                   << " ret = " << ret
                   << ", uuid = " << task->GetUuid();
        HandleCreateSnapshotError(task);
    }
    LOG(INFO) << "CreateSnapshot Canceled Success.";
    task->Finish();
    return;
}

void SnapshotCoreImpl::HandleCreateSnapshotError(
    std::shared_ptr<SnapshotTaskInfo> task) {
    SnapshotInfo &info = task->GetSnapshotInfo();
    info.SetStatus(Status::error);
    metaStore_->UpdateSnapshot(info);
    task->Finish();
    LOG(ERROR) << "CreateSnapshot fail.";
    return;
}

int SnapshotCoreImpl::CreateSnapshotOnCurvefs(
    const std::string &fileName,
    SnapshotInfo *info,
    std::shared_ptr<SnapshotTaskInfo> task) {
    uint64_t seqNum = 0;
    int ret =
        client_->CreateSnapshot(fileName, info->GetUser(), &seqNum);
    if (ret != LIBCURVE_ERROR::OK && ret != -LIBCURVE_ERROR::UNDER_SNAPSHOT) {
        LOG(ERROR) << "CreateSnapshot on curvefs fail, "
                   << " ret = " << ret;
        return kErrCodeInternalError;
    }
    LOG(INFO) << "CreateSnapshot on curvefs success, seq = " << seqNum;

    FInfo snapInfo;
    ret = client_->GetSnapshot(fileName,
        info->GetUser(),
        seqNum, &snapInfo);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(ERROR) << "GetSnapShot on curvefs fail, "
                   << " ret = " << ret
                   << ", fileName = " << fileName
                   << ", user = " << info->GetUser()
                   << ", seqNum = " << seqNum;
        return kErrCodeInternalError;
    }
    info->SetSeqNum(seqNum);
    info->SetChunkSize(snapInfo.chunksize);
    info->SetSegmentSize(snapInfo.segmentsize);
    info->SetFileLength(snapInfo.length);
    info->SetCreateTime(snapInfo.ctime);

    ret = metaStore_->UpdateSnapshot(*info);
    if (ret < 0) {
        LOG(ERROR) << "UpdateSnapshot error, "
                   << " ret = " << ret
                   << ", fileName = " << fileName;
        return ret;
    }

    // 打完快照需等待2个session时间，以保证seq同步到所有client
    std::this_thread::sleep_for(
        std::chrono::microseconds(mdsSessionTimeUs_ * 2));

    return kErrCodeSuccess;
}

int SnapshotCoreImpl::DeleteSnapshotOnCurvefs(const SnapshotInfo &info) {
    std::string fileName = info.GetFileName();
    std::string user = info.GetUser();
    uint64_t seqNum = info.GetSeqNum();
    int ret = client_->DeleteSnapshot(fileName,
        user,
        seqNum);
    if (ret != LIBCURVE_ERROR::OK &&
        ret != -LIBCURVE_ERROR::NOTEXIST &&
        ret != -LIBCURVE_ERROR::DELETING) {
        LOG(ERROR) << "DeleteSnapshot error, "
                   << " ret = " << ret
                   << ", fileName = " << fileName
                   << ", user = " << user
                   << ", seqNum = " << seqNum;
        return kErrCodeInternalError;
    }
    do {
        FileStatus status;
        ret = client_->CheckSnapShotStatus(info.GetFileName(),
            info.GetUser(),
            seqNum,
            &status);
            if (-LIBCURVE_ERROR::NOTEXIST == ret) {
                break;
            } else if (LIBCURVE_ERROR::OK == ret) {
                // nothing
                if (status != FileStatus::Deleting) {
                    break;
                }
            } else {
                LOG(ERROR) << "CheckSnapShotStatus fail"
                           << ", ret = " << ret;
                return kErrCodeInternalError;
            }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(checkSnapshotStatusIntervalMs_));
    } while (LIBCURVE_ERROR::OK == ret);
    return kErrCodeSuccess;
}

int SnapshotCoreImpl::BuildChunkIndexData(
    const SnapshotInfo &info,
    ChunkIndexData *indexData,
    std::map<uint64_t, SegmentInfo> *segInfos,
    std::shared_ptr<SnapshotTaskInfo> task) {
    std::string fileName = info.GetFileName();
    std::string user = info.GetUser();
    uint64_t seqNum = info.GetSeqNum();
    uint64_t fileLength = info.GetFileLength();
    uint64_t segmentSize = info.GetSegmentSize();
    uint64_t chunkSize = info.GetChunkSize();

    indexData->SetFileName(fileName);

    uint64_t chunkIndex = 0;
    for (uint64_t i = 0; i < fileLength/segmentSize; i++) {
        uint64_t offset = i * segmentSize;
        SegmentInfo segInfo;
        int ret = client_->GetSnapshotSegmentInfo(
            fileName,
            user,
            seqNum,
            offset,
            &segInfo);

        if (LIBCURVE_ERROR::OK == ret) {
            segInfos->emplace(i, segInfo);
            for (std::vector<uint64_t>::size_type j = 0;
                j < segInfo.chunkvec.size();
                j++) {
                ChunkInfoDetail chunkInfo;
                ChunkIDInfo cidInfo = segInfo.chunkvec[j];
                ret = client_->GetChunkInfo(cidInfo,
                    &chunkInfo);
                if (ret != LIBCURVE_ERROR::OK) {
                    LOG(ERROR) << "GetChunkInfo error, "
                               << " ret = " << ret
                               << ", logicalPoolId = " << cidInfo.lpid_
                               << ", copysetId = " << cidInfo.cpid_
                               << ", chunkId = " << cidInfo.cid_;
                    return kErrCodeInternalError;
                }
                // 2个sn，小的是snap sn，大的是快照之后的写
                // 1个sn，有两种情况：
                //    小于等于seqNum时为snap sn, 且快照之后未写过;
                //    大于时, 表示打快照时为空，是快照之后首次写的版本(seqNum+1)
                // 没有sn，从未写过
                // 大于2个sn，错误，报错
                if (chunkInfo.chunkSn.size() == 2) {
                    uint64_t seq =
                        std::min(chunkInfo.chunkSn[0],
                                chunkInfo.chunkSn[1]);
                    chunkIndex = i * (segmentSize / chunkSize) + j;
                    ChunkDataName chunkDataName(fileName, seq, chunkIndex);
                    indexData->PutChunkDataName(chunkDataName);
                } else if (chunkInfo.chunkSn.size() == 1) {
                    uint64_t seq = chunkInfo.chunkSn[0];
                    if (seq <= seqNum) {
                        chunkIndex = i * (segmentSize / chunkSize) + j;
                        ChunkDataName chunkDataName(fileName, seq, chunkIndex);
                        indexData->PutChunkDataName(chunkDataName);
                    }
                } else if (chunkInfo.chunkSn.size() == 0) {
                    // nothing
                } else {
                    // should not reach here
                    LOG(ERROR) << "GetChunkInfo return chunkInfo.chunkSn.size()"
                               << " invalid, size = "
                               << chunkInfo.chunkSn.size();
                    return kErrCodeInternalError;
                }
                if (task->IsCanceled()) {
                    return kErrCodeSuccess;
                }
            }
        } else if (-LIBCURVE_ERROR::NOT_ALLOCATE == ret) {
            // nothing
        } else {
            LOG(ERROR) << "GetSnapshotSegmentInfo error,"
                       << " ret = " << ret
                       << ", fileName = " << fileName
                       << ", user = " << user
                       << ", seq = " << seqNum
                       << ", offset = " << offset;
            return kErrCodeInternalError;
        }
    }

    return kErrCodeSuccess;
}

int SnapshotCoreImpl::BuildSegmentInfo(
    const SnapshotInfo &info,
    std::map<uint64_t, SegmentInfo> *segInfos) {
    int ret = kErrCodeSuccess;
    std::string fileName = info.GetFileName();
    std::string user = info.GetUser();
    uint64_t seq = info.GetSeqNum();
    uint64_t fileLength = info.GetFileLength();
    uint64_t segmentSize = info.GetSegmentSize();
    for (uint64_t i = 0;
        i < fileLength/segmentSize;
        i++) {
        uint64_t offset = i * segmentSize;
        SegmentInfo segInfo;
        ret = client_->GetSnapshotSegmentInfo(
            fileName,
            user,
            seq,
            offset,
            &segInfo);

        if (LIBCURVE_ERROR::OK == ret) {
            segInfos->emplace(i, std::move(segInfo));
        } else if (-LIBCURVE_ERROR::NOT_ALLOCATE == ret) {
            // nothing
        } else {
            LOG(ERROR) << "GetSnapshotSegmentInfo error,"
                       << " ret = " << ret
                       << ", fileName = " << fileName
                       << ", user = " << user
                       << ", seq = " << seq
                       << ", offset = " << offset;
            return kErrCodeInternalError;
        }
    }
    return kErrCodeSuccess;
}

int SnapshotCoreImpl::TransferSnapshotData(
    const ChunkIndexData indexData,
    const SnapshotInfo &info,
    const std::map<uint64_t, SegmentInfo> &segInfos,
    const ChunkDataExistFilter &filter,
    std::shared_ptr<SnapshotTaskInfo> task) {
    int ret = 0;
    uint64_t segmentSize = info.GetSegmentSize();
    uint64_t chunkSize = info.GetChunkSize();
    uint64_t chunkPerSegment = segmentSize/chunkSize;

    if (0 == chunkSplitSize_ || chunkSize % chunkSplitSize_ != 0) {
        LOG(ERROR) << "error!, ChunkSize is not align to chunkSplitSize.";
        return kErrCodeChunkSizeNotAligned;
    }

    std::vector<ChunkIndexType> chunkIndexVec = indexData.GetAllChunkIndex();

    uint32_t totalProgress = kProgressTransferSnapshotDataComplete -
        kProgressTransferSnapshotDataStart;
    uint32_t transferDataNum = chunkIndexVec.size();
    double progressPerData =
        static_cast<double>(totalProgress) / transferDataNum;
    uint32_t index = 0;

    for (auto &chunkIndex : chunkIndexVec) {
        uint64_t segNum = chunkIndex / chunkPerSegment;

        auto it = segInfos.find(segNum);
        if (it == segInfos.end()) {
            LOG(ERROR) << "TransferSnapshotData has encounter an interanl error"
                       << ": The ChunkIndexData is not match to SegmentInfo!!!"
                       << " chunkIndex = " << chunkIndex
                       << ", segNum = " << segNum;
            return kErrCodeInternalError;
        }

        uint64_t chunkIndexInSegment = chunkIndex % chunkPerSegment;
        if (chunkIndexInSegment >= it->second.chunkvec.size()) {
            LOG(ERROR) << "TransferSnapshotData, "
                       << "chunkIndexInSegment >= "
                       << "segInfos[segNum].chunkvec.size()"
                       << ", chunkIndexInSegment = "
                       << chunkIndexInSegment
                       << ", size = "
                       << it->second.chunkvec.size();
            return kErrCodeInternalError;
        }
    }

    auto tracker = std::make_shared<TaskTracker>();
    for (auto &chunkIndex : chunkIndexVec) {
        ChunkDataName chunkDataName;
        indexData.GetChunkDataName(chunkIndex, &chunkDataName);
        uint64_t segNum = chunkIndex / chunkPerSegment;
        uint64_t chunkIndexInSegment = chunkIndex % chunkPerSegment;

        auto it = segInfos.find(segNum);
        if (it != segInfos.end()) {
            ChunkIDInfo cidInfo =
                it->second.chunkvec[chunkIndexInSegment];
            if (!filter(chunkDataName)) {
                auto taskInfo =
                    std::make_shared<TransferSnapshotDataChunkTaskInfo>(
                        chunkDataName, chunkSize, cidInfo, chunkSplitSize_);
                UUID taskId = UUIDGenerator().GenerateUUID();
                auto task = new TransferSnapshotDataChunkTask(
                    taskId,
                    taskInfo,
                    client_,
                    dataStore_);
                tracker->AddTask(task);
                threadPool_->PushTask(task);
            }
        }
        if (tracker->GetTaskNum() >= snapshotCoreThreadNum_) {
            tracker->WaitSome(1);
        }
        ret = tracker->GetResult();
        if (ret < 0) {
            LOG(ERROR) << "TransferSnapshotDataChunk tracker GetResult fail"
                       << ", ret = " << ret;
            return ret;
        }

        task->SetProgress(static_cast<uint32_t>(
                kProgressTransferSnapshotDataStart + index * progressPerData));
        task->UpdateMetric();
        index++;
        if (task->IsCanceled()) {
            return kErrCodeSuccess;
        }
    }
    // 最后剩余数量不足的任务
    tracker->Wait();
    ret = tracker->GetResult();
    if (ret < 0) {
        LOG(ERROR) << "TransferSnapshotDataChunk tracker GetResult fail"
                   << ", ret = " << ret;
        return ret;
    }

    ret = DeleteSnapshotOnCurvefs(info);
    if (ret < 0) {
        LOG(ERROR) << "DeleteSnapshotOnCurvefs fail.";
        return ret;
    }
    return kErrCodeSuccess;
}


int SnapshotCoreImpl::DeleteSnapshotPre(
    UUID uuid,
    const std::string &user,
    const std::string &fileName,
    SnapshotInfo *snapInfo) {
    NameLockGuard lockSnapGuard(snapshotRef_->GetSnapshotLock(), uuid);
    int ret = metaStore_->GetSnapshotInfo(uuid, snapInfo);
    if (ret < 0) {
        // 快照不存在时直接返回删除成功，使接口幂等
        return kErrCodeSuccess;
    }
    if (snapInfo->GetUser() != user) {
        LOG(ERROR) << "Can not delete snapshot by different user.";
        return kErrCodeInvalidUser;
    }
    if (fileName != snapInfo->GetFileName()) {
        LOG(ERROR) << "Can not delete, fileName is not matched.";
        return kErrCodeFileNameNotMatch;
    }

    switch (snapInfo->GetStatus()) {
        case Status::done:
            snapInfo->SetStatus(Status::deleting);
            break;
        case Status::error:
            snapInfo->SetStatus(Status::errorDeleting);
            break;
        case Status::canceling:
        case Status::deleting:
        case Status::errorDeleting:
            return kErrCodeTaskExist;
        case Status::pending:
            return kErrCodeSnapshotCannotDeleteUnfinished;
        default:
            LOG(ERROR) << "Can not reach here!";
            return kErrCodeInternalError;
    }

    if (snapshotRef_->GetSnapshotRef(uuid) > 0) {
        return kErrCodeSnapshotCannotDeleteCloning;
    }

    ret = metaStore_->UpdateSnapshot(*snapInfo);
    if (ret < 0) {
        LOG(ERROR) << "UpdateSnapshot error,"
                   << " ret = " << ret
                   << ", uuid = " << uuid;
        return ret;
    }
    return kErrCodeSuccess;
}

constexpr uint32_t kDelProgressBuildSnapshotMapComplete = 10;
constexpr uint32_t kDelProgressDeleteChunkDataStart =
                       kDelProgressBuildSnapshotMapComplete;
constexpr uint32_t kDelProgressDeleteChunkDataComplete = 80;
constexpr uint32_t kDelProgressDeleteChunkIndexDataComplete = 90;

/**
 * @brief 异步执行删除快照任务并更新任务进度
 *
 * 删除快照进度规划如下：
 *
 * |BuildSnapshotMap|DeleteChunkData|DeleteChunkIndexData|DeleteSnapshot|
 * | 10%            | 10%~80%       | 90%                | 100%         |
 *
 * @param task 快照任务
 */
void SnapshotCoreImpl::HandleDeleteSnapshotTask(
    std::shared_ptr<SnapshotTaskInfo> task) {
    SnapshotInfo &info = task->GetSnapshotInfo();
    UUID uuid = task->GetUuid();
    uint64_t seqNum = info.GetSeqNum();
    FileSnapMap fileSnapshotMap;
    int ret = BuildSnapshotMap(task->GetFileName(), seqNum, &fileSnapshotMap);
    if (ret < 0) {
        LOG(ERROR) << "BuildSnapshotMap error, "
                   << " fileName = " << task->GetFileName()
                   << ", seqNum = " << seqNum;
        HandleDeleteSnapshotError(task);
        return;
    }
    task->SetProgress(kDelProgressBuildSnapshotMapComplete);
    task->UpdateMetric();
    ChunkIndexDataName name(task->GetFileName(),
        seqNum);
    ChunkIndexData indexData;
    if (dataStore_->ChunkIndexDataExist(name)) {
        ret = dataStore_->GetChunkIndexData(name, &indexData);
        if (ret < 0) {
            LOG(ERROR) << "GetChunkIndexData error ,"
                       << " fileName = " << task->GetFileName()
                       << ", seqNum = " << seqNum;
            HandleDeleteSnapshotError(task);
            return;
        }

        auto chunkIndexVec = indexData.GetAllChunkIndex();

        uint32_t totalProgress = kDelProgressDeleteChunkDataComplete -
            kDelProgressDeleteChunkDataStart;
        uint32_t chunkDataNum = chunkIndexVec.size();
        double progressPerData = static_cast<double> (totalProgress) /
            chunkDataNum;
        uint32_t index = 0;

        for (auto &chunkIndex : chunkIndexVec) {
            ChunkDataName chunkDataName;
            indexData.GetChunkDataName(chunkIndex, &chunkDataName);
            if ((!fileSnapshotMap.IsExistChunk(chunkDataName)) &&
                (dataStore_->ChunkDataExist(chunkDataName))) {
                ret =  dataStore_->DeleteChunkData(chunkDataName);
                if (ret < 0) {
                    LOG(ERROR) << "DeleteChunkData error, "
                               << " ret = " << ret
                               << ", fileName = " << task->GetFileName()
                               << ", seqNum = " << seqNum
                               << ", chunkIndex = "
                               << chunkDataName.chunkIndex_;
                    HandleDeleteSnapshotError(task);
                    return;
                }
            }
            task->SetProgress(static_cast<uint32_t>(
                kDelProgressDeleteChunkDataStart + index * progressPerData));
            task->UpdateMetric();
            index++;
        }
        task->SetProgress(kDelProgressDeleteChunkDataComplete);
        ret = dataStore_->DeleteChunkIndexData(name);
        if (ret < 0) {
            LOG(ERROR) << "DeleteChunkIndexData error, "
                       << " ret = " << ret
                       << ", fileName = " << task->GetFileName()
                       << ", seqNum = " << seqNum;
            HandleDeleteSnapshotError(task);
            return;
        }
    }
    // ClearSnapshotOnCurvefs  when errorDeleting
    if ((Status::errorDeleting == info.GetStatus()) ||
        (Status::canceling == info.GetStatus())) {
        ret = DeleteSnapshotOnCurvefs(info);
        if (ret < 0) {
            LOG(ERROR) << "DeleteSnapshotOnCurvefs fail.";
            HandleDeleteSnapshotError(task);
            return;
        }
    }

    task->SetProgress(kDelProgressDeleteChunkIndexDataComplete);
    task->UpdateMetric();
    ret = metaStore_->DeleteSnapshot(uuid);
    if (ret < 0) {
        LOG(ERROR) << "DeleteSnapshot error, "
                   << " ret = " << ret
                   << ", uuid = " << uuid;
        HandleDeleteSnapshotError(task);
        return;
    }

    task->SetProgress(kProgressComplete);
    task->Finish();
    return;
}


void SnapshotCoreImpl::HandleDeleteSnapshotError(
    std::shared_ptr<SnapshotTaskInfo> task) {
    SnapshotInfo &info = task->GetSnapshotInfo();
    LOG(ERROR) << "HandleDeleteSnapshotTask fail.";
    info.SetStatus(Status::error);
    metaStore_->UpdateSnapshot(info);
    task->Finish();
    return;
}

int SnapshotCoreImpl::GetFileSnapshotInfo(const std::string &file,
    std::vector<SnapshotInfo> *info) {
    metaStore_->GetSnapshotList(file, info);
    return kErrCodeSuccess;
}

int SnapshotCoreImpl::GetSnapshotInfo(const UUID uuid,
    SnapshotInfo *info) {
    return metaStore_->GetSnapshotInfo(uuid, info);
}

int SnapshotCoreImpl::BuildSnapshotMap(const std::string &fileName,
    uint64_t seqNum,
    FileSnapMap *fileSnapshotMap) {
    std::vector<SnapshotInfo> snapInfos;
    int ret = metaStore_->GetSnapshotList(fileName, &snapInfos);
    for (auto &snap : snapInfos) {
        if (snap.GetSeqNum() != seqNum) {
            ChunkIndexDataName name(snap.GetFileName(), snap.GetSeqNum());
            ChunkIndexData indexData;
            ret = dataStore_->GetChunkIndexData(name, &indexData);
            if (ret < 0) {
                LOG(ERROR) << "GetChunkIndexData error, "
                           << " ret = " << ret
                           << ", fileName = " <<  snap.GetFileName()
                           << ", seqNum = " << snap.GetSeqNum();
                // 此处不能返回错误，
                // 否则一旦某个失败的快照没有indexdata，所有快照都无法删除
            } else {
                fileSnapshotMap->maps.push_back(std::move(indexData));
            }
        }
    }
    return kErrCodeSuccess;
}

int SnapshotCoreImpl::GetSnapshotList(std::vector<SnapshotInfo> *list) {
    return metaStore_->GetSnapshotList(list);
}

}  // namespace snapshotcloneserver
}  // namespace curve

