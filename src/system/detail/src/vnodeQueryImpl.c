/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "hash.h"
#include "hashutil.h"
#include "taosmsg.h"
#include "textbuffer.h"
#include "ttime.h"

#include "tinterpolation.h"
#include "tscJoinProcess.h"
#include "tscSecondaryMerge.h"
#include "tscompression.h"
#include "ttime.h"
#include "vnode.h"
#include "vnodeRead.h"
#include "vnodeUtil.h"

#include "vnodeCache.h"
#include "vnodeDataFilterFunc.h"
#include "vnodeFile.h"
#include "vnodeQueryImpl.h"
#include "vnodeStatus.h"

enum {
  TS_JOIN_TS_EQUAL = 0,
  TS_JOIN_TS_NOT_EQUALS = 1,
  TS_JOIN_TAG_NOT_EQUALS = 2,
};

enum {
  DISK_BLOCK_NO_NEED_TO_LOAD = 0,
  DISK_BLOCK_LOAD_TS = 1,
  DISK_BLOCK_LOAD_BLOCK = 2,
};

#define IS_DISK_DATA_BLOCK(q) ((q)->fileId >= 0)

static int32_t readDataFromDiskFile(int fd, SQInfo *pQInfo, SQueryFilesInfo *pQueryFile, char *buf, uint64_t offset,
                                    int32_t size);

static void    vnodeInitLoadCompBlockInfo(SLoadCompBlockInfo *pCompBlockLoadInfo);
static int32_t moveToNextBlock(SQueryRuntimeEnv *pRuntimeEnv, int32_t step, __block_search_fn_t searchFn,
                               bool loadData);
static int32_t doMergeMetersResultsToGroupRes(SMeterQuerySupportObj *pSupporter, SQuery *pQuery,
                                              SQueryRuntimeEnv *pRuntimeEnv, SMeterDataInfo *pMeterHeadDataInfo,
                                              int32_t start, int32_t end);

static TSKEY getTimestampInCacheBlock(SQueryRuntimeEnv *pRuntimeEnv, SCacheBlock *pBlock, int32_t index);
static TSKEY getTimestampInDiskBlock(SQueryRuntimeEnv *pRuntimeEnv, int32_t index);

static void    savePointPosition(SPositionInfo *position, int32_t fileId, int32_t slot, int32_t pos);
static int32_t getNextDataFileCompInfo(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, int32_t step);

static void setGroupOutputBuffer(SQueryRuntimeEnv *pRuntimeEnv, SOutputRes *pResult);

static void getAlignedIntervalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, TSKEY keyInData, TSKEY skey, TSKEY ekey);
static void doApplyIntervalQueryOnBlock(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo,
                                        SBlockInfo *pBlockInfo, int64_t *pPrimaryCol, SField *pFields,
                                        __block_search_fn_t searchFn);

static int32_t saveResult(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo, int32_t numOfResult);
static void applyIntervalQueryOnBlock(SMeterQuerySupportObj *pSupporter, SMeterDataInfo *pMeterDataInfo,
                                      SBlockInfo *pBlockInfo, int32_t blockStatus, SField *pFields,
                                      __block_search_fn_t searchFn);

static void    resetMergeResultBuf(SQuery *pQuery, SQLFunctionCtx *pCtx);
static int32_t flushFromResultBuf(SMeterQuerySupportObj *pSupporter, const SQuery *pQuery,
                                  const SQueryRuntimeEnv *pRuntimeEnv);
static void    validateTimestampForSupplementResult(SQueryRuntimeEnv *pRuntimeEnv, int64_t numOfIncrementRes);
static void    getBasicCacheInfoSnapshot(SQuery *pQuery, SCacheInfo *pCacheInfo, int32_t vid);
static TSKEY   getQueryPositionForCacheInvalid(SQueryRuntimeEnv *pRuntimeEnv, __block_search_fn_t searchFn);
static bool    functionNeedToExecute(SQueryRuntimeEnv *pRuntimeEnv, SQLFunctionCtx *pCtx, int32_t functionId);
static void    doGetAlignedIntervalQueryRangeImpl(SQuery *pQuery, int64_t pKey, int64_t keyFirst, int64_t keyLast,
                                                  int64_t *actualSkey, int64_t *actualEkey, int64_t *skey, int64_t *ekey);
static void    getNextLogicalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, STimeWindow* pTimeWindow);

// check the offset value integrity
static FORCE_INLINE int32_t validateHeaderOffsetSegment(SQInfo *pQInfo, char *filePath, int32_t vid, char *data,
                                                        int32_t size) {
  if (!taosCheckChecksumWhole((uint8_t *)data + TSDB_FILE_HEADER_LEN, size)) {
    dLError("QInfo:%p vid:%d, failed to read header file:%s, file offset area is broken", pQInfo, vid, filePath);
    return -1;
  }
  return 0;
}

static FORCE_INLINE int32_t getCompHeaderSegSize(SVnodeCfg *pCfg) {
  return pCfg->maxSessions * sizeof(SCompHeader) + sizeof(TSCKSUM);
}

static FORCE_INLINE int32_t getCompHeaderStartPosition(SVnodeCfg *pCfg) {
  return TSDB_FILE_HEADER_LEN + getCompHeaderSegSize(pCfg);
}

static FORCE_INLINE int32_t validateCompBlockOffset(SQInfo *pQInfo, SMeterObj *pMeterObj, SCompHeader *pCompHeader,
                                                    SQueryFilesInfo *pQueryFileInfo, int32_t headerSize) {
  if (pCompHeader->compInfoOffset < headerSize || pCompHeader->compInfoOffset > pQueryFileInfo->headerFileSize) {
    dError("QInfo:%p vid:%d sid:%d id:%s, compInfoOffset:%" PRId64 " is not valid, size:%" PRId64, pQInfo,
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pCompHeader->compInfoOffset,
           pQueryFileInfo->headerFileSize);

    return -1;
  }

  return 0;
}

// check compinfo integrity
static FORCE_INLINE int32_t validateCompBlockInfoSegment(SQInfo *pQInfo, const char *filePath, int32_t vid,
                                                         SCompInfo *compInfo, int64_t offset) {
  if (!taosCheckChecksumWhole((uint8_t *)compInfo, sizeof(SCompInfo))) {
    dLError("QInfo:%p vid:%d, failed to read header file:%s, file compInfo broken, offset:%" PRId64, pQInfo, vid,
            filePath, offset);
    return -1;
  }
  return 0;
}

static FORCE_INLINE int32_t validateCompBlockSegment(SQInfo *pQInfo, const char *filePath, SCompInfo *compInfo,
                                                     char *pBlock, int32_t vid, TSCKSUM checksum) {
  uint32_t size = compInfo->numOfBlocks * sizeof(SCompBlock);

  if (checksum != taosCalcChecksum(0, (uint8_t *)pBlock, size)) {
    dLError("QInfo:%p vid:%d, failed to read header file:%s, file compblock is broken:%zu", pQInfo, vid, filePath,
            (char *)compInfo + sizeof(SCompInfo));
    return -1;
  }

  return 0;
}

bool isGroupbyNormalCol(SSqlGroupbyExpr *pGroupbyExpr) {
  if (pGroupbyExpr == NULL || pGroupbyExpr->numOfGroupCols == 0) {
    return false;
  }

  for (int32_t i = 0; i < pGroupbyExpr->numOfGroupCols; ++i) {
    SColIndexEx *pColIndex = &pGroupbyExpr->columnInfo[i];
    if (pColIndex->flag == TSDB_COL_NORMAL) {
      /*
       * make sure the normal column locates at the second position if tbname exists in group by clause
       */
      if (pGroupbyExpr->numOfGroupCols > 1) {
        assert(pColIndex->colIdx > 0);
      }

      return true;
    }
  }

  return false;
}

int16_t getGroupbyColumnType(SQuery *pQuery, SSqlGroupbyExpr *pGroupbyExpr) {
  assert(pGroupbyExpr != NULL);

  int32_t colId = -2;
  int16_t type = TSDB_DATA_TYPE_NULL;

  for (int32_t i = 0; i < pGroupbyExpr->numOfGroupCols; ++i) {
    SColIndexEx *pColIndex = &pGroupbyExpr->columnInfo[i];
    if (pColIndex->flag == TSDB_COL_NORMAL) {
      colId = pColIndex->colId;
      break;
    }
  }

  for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
    if (colId == pQuery->colList[i].data.colId) {
      type = pQuery->colList[i].data.type;
      break;
    }
  }

  return type;
}

bool isSelectivityWithTagsQuery(SQuery *pQuery) {
  bool    hasTags = false;
  int32_t numOfSelectivity = 0;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functId == TSDB_FUNC_TAG_DUMMY || functId == TSDB_FUNC_TS_DUMMY) {
      hasTags = true;
      continue;
    }

    if ((aAggs[functId].nStatus & TSDB_FUNCSTATE_SELECTIVITY) != 0) {
      numOfSelectivity++;
    }
  }

  if (numOfSelectivity > 0 && hasTags) {
    return true;
  }

  return false;
}

static void vnodeFreeFieldsEx(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  vnodeFreeFields(pQuery);

  vnodeInitLoadCompBlockInfo(&pRuntimeEnv->loadCompBlockInfo);
}

static bool vnodeIsCompBlockInfoLoaded(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, int32_t fileIndex) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  // check if data file header of this table has been loaded into memory, avoid to reloaded comp Block info
  SLoadCompBlockInfo *pLoadCompBlockInfo = &pRuntimeEnv->loadCompBlockInfo;

  // if vnodeFreeFields is called, the pQuery->pFields is NULL
  if (pLoadCompBlockInfo->fileListIndex == fileIndex && pLoadCompBlockInfo->sid == pMeterObj->sid &&
      pQuery->pFields != NULL && pQuery->fileId > 0) {
    assert(pRuntimeEnv->vnodeFileInfo.pFileInfo[fileIndex].fileID == pLoadCompBlockInfo->fileId &&
           pQuery->numOfBlocks > 0);
    return true;
  }

  return false;
}

static void vnodeSetCompBlockInfoLoaded(SQueryRuntimeEnv *pRuntimeEnv, int32_t fileIndex, int32_t sid) {
  SLoadCompBlockInfo *pCompBlockLoadInfo = &pRuntimeEnv->loadCompBlockInfo;

  pCompBlockLoadInfo->sid = sid;
  pCompBlockLoadInfo->fileListIndex = fileIndex;
  pCompBlockLoadInfo->fileId = pRuntimeEnv->vnodeFileInfo.pFileInfo[fileIndex].fileID;
}

static void vnodeInitLoadCompBlockInfo(SLoadCompBlockInfo *pCompBlockLoadInfo) {
  pCompBlockLoadInfo->sid = -1;
  pCompBlockLoadInfo->fileId = -1;
  pCompBlockLoadInfo->fileListIndex = -1;
}

static int32_t vnodeIsDatablockLoaded(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, int32_t fileIndex,
                                      bool loadPrimaryTS) {
  SQuery *            pQuery = pRuntimeEnv->pQuery;
  SLoadDataBlockInfo *pLoadInfo = &pRuntimeEnv->loadBlockInfo;

  /* this block has been loaded into memory, return directly */
  if (pLoadInfo->fileId == pQuery->fileId && pLoadInfo->slotIdx == pQuery->slot && pQuery->slot != -1 &&
      pLoadInfo->sid == pMeterObj->sid && pLoadInfo->fileListIndex == fileIndex) {
    // previous load operation does not load the primary timestamp column, we only need to load the timestamp column
    if (pLoadInfo->tsLoaded == false && pLoadInfo->tsLoaded != loadPrimaryTS) {
      return DISK_BLOCK_LOAD_TS;
    } else {
      return DISK_BLOCK_NO_NEED_TO_LOAD;
    }
  }

  return DISK_BLOCK_LOAD_BLOCK;
}

static void vnodeSetDataBlockInfoLoaded(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, int32_t fileIndex,
                                        bool tsLoaded) {
  SQuery *            pQuery = pRuntimeEnv->pQuery;
  SLoadDataBlockInfo *pLoadInfo = &pRuntimeEnv->loadBlockInfo;

  pLoadInfo->fileId = pQuery->fileId;
  pLoadInfo->slotIdx = pQuery->slot;
  pLoadInfo->fileListIndex = fileIndex;
  pLoadInfo->sid = pMeterObj->sid;
  pLoadInfo->tsLoaded = tsLoaded;
}

static void vnodeInitDataBlockInfo(SLoadDataBlockInfo *pBlockLoadInfo) {
  pBlockLoadInfo->slotIdx = -1;
  pBlockLoadInfo->fileId = -1;
  pBlockLoadInfo->sid = -1;
  pBlockLoadInfo->fileListIndex = -1;
}

static void vnodeSetCurrentFileNames(SQueryFilesInfo *pVnodeFilesInfo) {
  assert(pVnodeFilesInfo->current >= 0 && pVnodeFilesInfo->current < pVnodeFilesInfo->numOfFiles);

  SHeaderFileInfo *pCurrentFileInfo = &pVnodeFilesInfo->pFileInfo[pVnodeFilesInfo->current];

  /*
   * set the full file path for current opened files
   * the maximum allowed path string length is PATH_MAX in Linux, 100 bytes is used to
   * suppress the compiler warnings
   */
  char    str[PATH_MAX + 100] = {0};
  int32_t PATH_WITH_EXTRA = PATH_MAX + 100;

  int32_t vnodeId = pVnodeFilesInfo->vnodeId;
  int32_t fileId = pCurrentFileInfo->fileID;

  int32_t len = snprintf(str, PATH_WITH_EXTRA, "%sv%df%d.head", pVnodeFilesInfo->dbFilePathPrefix, vnodeId, fileId);
  assert(len <= PATH_MAX);

  strncpy(pVnodeFilesInfo->headerFilePath, str, PATH_MAX);

  len = snprintf(str, PATH_WITH_EXTRA, "%sv%df%d.data", pVnodeFilesInfo->dbFilePathPrefix, vnodeId, fileId);
  assert(len <= PATH_MAX);

  strncpy(pVnodeFilesInfo->dataFilePath, str, PATH_MAX);

  len = snprintf(str, PATH_WITH_EXTRA, "%sv%df%d.last", pVnodeFilesInfo->dbFilePathPrefix, vnodeId, fileId);
  assert(len <= PATH_MAX);

  strncpy(pVnodeFilesInfo->lastFilePath, str, PATH_MAX);
}

/**
 * if the header is smaller than a threshold value(header size + initial offset value)
 *
 * @param vnodeId
 * @param headerFileSize
 * @return
 */
static FORCE_INLINE bool isHeaderFileEmpty(int32_t vnodeId, size_t headerFileSize) {
  SVnodeCfg *pVnodeCfg = &vnodeList[vnodeId].cfg;
  return headerFileSize <= getCompHeaderStartPosition(pVnodeCfg);
}

static bool checkIsHeaderFileEmpty(SQueryFilesInfo *pVnodeFilesInfo) {
  struct stat fstat = {0};
  if (stat(pVnodeFilesInfo->headerFilePath, &fstat) < 0) {
    return true;
  }

  pVnodeFilesInfo->headerFileSize = fstat.st_size;
  return isHeaderFileEmpty(pVnodeFilesInfo->vnodeId, pVnodeFilesInfo->headerFileSize);
}

static void doCloseQueryFileInfoFD(SQueryFilesInfo *pVnodeFilesInfo) {
  tclose(pVnodeFilesInfo->headerFd);
  tclose(pVnodeFilesInfo->dataFd);
  tclose(pVnodeFilesInfo->lastFd);

  pVnodeFilesInfo->current = -1;
  pVnodeFilesInfo->headerFileSize = -1;
}

static void doInitQueryFileInfoFD(SQueryFilesInfo *pVnodeFilesInfo) {
  pVnodeFilesInfo->current = -1;
  pVnodeFilesInfo->headerFileSize = -1;

  pVnodeFilesInfo->headerFd = FD_INITIALIZER;  // set the initial value
  pVnodeFilesInfo->dataFd = FD_INITIALIZER;
  pVnodeFilesInfo->lastFd = FD_INITIALIZER;
}

/*
 * close the opened fd are delegated to invoker
 */
static int32_t doOpenQueryFile(SQInfo *pQInfo, SQueryFilesInfo *pVnodeFileInfo) {
  SHeaderFileInfo *pHeaderFileInfo = &pVnodeFileInfo->pFileInfo[pVnodeFileInfo->current];

  /*
   * current header file is empty or broken, return directly.
   *
   * if the header is smaller than or equals to the minimum file size value, this file is empty. No need to open this
   * file and the corresponding files.
   */
  if (checkIsHeaderFileEmpty(pVnodeFileInfo)) {
    qTrace("QInfo:%p vid:%d, fileId:%d, index:%d, size:%d, ignore file, empty or broken", pQInfo,
           pVnodeFileInfo->vnodeId, pHeaderFileInfo->fileID, pVnodeFileInfo->current, pVnodeFileInfo->headerFileSize);

    return -1;
  }

  pVnodeFileInfo->headerFd = open(pVnodeFileInfo->headerFilePath, O_RDONLY);
  if (!FD_VALID(pVnodeFileInfo->headerFd)) {
    dError("QInfo:%p failed open head file:%s reason:%s", pQInfo, pVnodeFileInfo->headerFilePath, strerror(errno));
    return -1;
  }

  pVnodeFileInfo->dataFd = open(pVnodeFileInfo->dataFilePath, O_RDONLY);
  if (!FD_VALID(pVnodeFileInfo->dataFd)) {
    dError("QInfo:%p failed open data file:%s reason:%s", pQInfo, pVnodeFileInfo->dataFilePath, strerror(errno));
    return -1;
  }

  pVnodeFileInfo->lastFd = open(pVnodeFileInfo->lastFilePath, O_RDONLY);
  if (!FD_VALID(pVnodeFileInfo->lastFd)) {
    dError("QInfo:%p failed open last file:%s reason:%s", pQInfo, pVnodeFileInfo->lastFilePath, strerror(errno));
    return -1;
  }

  return TSDB_CODE_SUCCESS;
}

static void doCloseQueryFiles(SQueryFilesInfo *pVnodeFileInfo) {
  if (pVnodeFileInfo->current >= 0) {
    assert(pVnodeFileInfo->current < pVnodeFileInfo->numOfFiles && pVnodeFileInfo->current >= 0);

    pVnodeFileInfo->headerFileSize = -1;

    doCloseQueryFileInfoFD(pVnodeFileInfo);
  }

  assert(pVnodeFileInfo->current == -1);
}

/**
 * For each query, only one header file along with corresponding files is opened, in order to
 * avoid too many memory files opened at the same time.
 *
 * @param pRuntimeEnv
 * @param fileIndex
 * @return   -1 failed, 0 success
 */
int32_t vnodeGetHeaderFile(SQueryRuntimeEnv *pRuntimeEnv, int32_t fileIndex) {
  assert(fileIndex >= 0 && fileIndex < pRuntimeEnv->vnodeFileInfo.numOfFiles);

  SQuery *pQuery = pRuntimeEnv->pQuery;
  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);  // only for log output

  SQueryFilesInfo *pVnodeFileInfo = &pRuntimeEnv->vnodeFileInfo;

  if (pVnodeFileInfo->current != fileIndex) {
    if (pVnodeFileInfo->current >= 0) {
      assert(pVnodeFileInfo->headerFileSize > 0);
    }

    // do close the current memory mapped header file and corresponding fd
    doCloseQueryFiles(pVnodeFileInfo);
    assert(pVnodeFileInfo->headerFileSize == -1);

    // set current opened file Index
    pVnodeFileInfo->current = fileIndex;

    // set the current opened files(header, data, last) path
    vnodeSetCurrentFileNames(pVnodeFileInfo);

    if (doOpenQueryFile(pQInfo, pVnodeFileInfo) != TSDB_CODE_SUCCESS) {
      doCloseQueryFiles(pVnodeFileInfo);  // all the fds may be partially opened, close them anyway.
      return -1;
    }
  }

  return TSDB_CODE_SUCCESS;
}

/*
 * read comp block info from header file
 *
 */
static int vnodeGetCompBlockInfo(SMeterObj *pMeterObj, SQueryRuntimeEnv *pRuntimeEnv, int32_t fileIndex) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  SVnodeCfg *      pCfg = &vnodeList[pMeterObj->vnode].cfg;
  SHeaderFileInfo *pHeadeFileInfo = &pRuntimeEnv->vnodeFileInfo.pFileInfo[fileIndex];

  int64_t st = taosGetTimestampUs();

  if (vnodeIsCompBlockInfoLoaded(pRuntimeEnv, pMeterObj, fileIndex)) {
    dTrace("QInfo:%p vid:%d sid:%d id:%s, fileId:%d compBlock info is loaded, not reload", GET_QINFO_ADDR(pQuery),
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pHeadeFileInfo->fileID);

    return pQuery->numOfBlocks;
  }

  SQueryCostSummary *pSummary = &pRuntimeEnv->summary;
  pSummary->readCompInfo++;
  pSummary->numOfSeek++;

  int32_t ret = vnodeGetHeaderFile(pRuntimeEnv, fileIndex);
  if (ret != TSDB_CODE_SUCCESS) {
    return -1;  // failed to load the header file data into memory
  }

  char *           buf = calloc(1, getCompHeaderSegSize(pCfg));
  SQueryFilesInfo *pVnodeFileInfo = &pRuntimeEnv->vnodeFileInfo;

  lseek(pVnodeFileInfo->headerFd, TSDB_FILE_HEADER_LEN, SEEK_SET);
  read(pVnodeFileInfo->headerFd, buf, getCompHeaderSegSize(pCfg));

  // check the offset value integrity
  if (validateHeaderOffsetSegment(pQInfo, pRuntimeEnv->vnodeFileInfo.headerFilePath, pMeterObj->vnode,
                                  buf - TSDB_FILE_HEADER_LEN, getCompHeaderSegSize(pCfg)) < 0) {
    free(buf);
    return -1;
  }

  SCompHeader *compHeader = (SCompHeader *)(buf + sizeof(SCompHeader) * pMeterObj->sid);

  // no data in this file for specified meter, abort
  if (compHeader->compInfoOffset == 0) {
    free(buf);
    return 0;
  }

  // corrupted file may cause the invalid compInfoOffset, check needs
  if (validateCompBlockOffset(pQInfo, pMeterObj, compHeader, &pRuntimeEnv->vnodeFileInfo,
                              getCompHeaderStartPosition(pCfg)) < 0) {
    free(buf);
    return -1;
  }

  lseek(pVnodeFileInfo->headerFd, compHeader->compInfoOffset, SEEK_SET);

  SCompInfo compInfo = {0};
  read(pVnodeFileInfo->headerFd, &compInfo, sizeof(SCompInfo));

  // check compblock info integrity
  if (validateCompBlockInfoSegment(pQInfo, pRuntimeEnv->vnodeFileInfo.headerFilePath, pMeterObj->vnode, &compInfo,
                                   compHeader->compInfoOffset) < 0) {
    free(buf);
    return -1;
  }

  if (compInfo.numOfBlocks <= 0 || compInfo.uid != pMeterObj->uid) {
    free(buf);
    return 0;
  }

  // free allocated SField data
  vnodeFreeFieldsEx(pRuntimeEnv);
  pQuery->numOfBlocks = (int32_t)compInfo.numOfBlocks;

  /*
   * +-------------+-----------+----------------+
   * | comp block  | checksum  | SField Pointer |
   * +-------------+-----------+----------------+
   */
  int32_t compBlockSize = compInfo.numOfBlocks * sizeof(SCompBlock);
  size_t  bufferSize = compBlockSize + sizeof(TSCKSUM) + POINTER_BYTES * pQuery->numOfBlocks;

  // prepare buffer to hold compblock data
  if (pQuery->blockBufferSize != bufferSize) {
    pQuery->pBlock = realloc(pQuery->pBlock, bufferSize);
    pQuery->blockBufferSize = (int32_t)bufferSize;
  }

  memset(pQuery->pBlock, 0, bufferSize);

  // read data: comp block + checksum
  read(pVnodeFileInfo->headerFd, pQuery->pBlock, compBlockSize + sizeof(TSCKSUM));
  TSCKSUM checksum = *(TSCKSUM *)((char *)pQuery->pBlock + compBlockSize);

  // check comp block integrity
  if (validateCompBlockSegment(pQInfo, pRuntimeEnv->vnodeFileInfo.headerFilePath, &compInfo, (char *)pQuery->pBlock,
                               pMeterObj->vnode, checksum) < 0) {
    free(buf);
    return -1;
  }

  pQuery->pFields = (SField **)((char *)pQuery->pBlock + compBlockSize + sizeof(TSCKSUM));
  vnodeSetCompBlockInfoLoaded(pRuntimeEnv, fileIndex, pMeterObj->sid);

  int64_t et = taosGetTimestampUs();
  qTrace("QInfo:%p vid:%d sid:%d id:%s, fileId:%d, load compblock info, size:%d, elapsed:%f ms", pQInfo,
         pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pRuntimeEnv->vnodeFileInfo.pFileInfo[fileIndex].fileID,
         compBlockSize, (et - st) / 1000.0);

  pSummary->totalCompInfoSize += compBlockSize;
  pSummary->loadCompInfoUs += (et - st);

  free(buf);
  return pQuery->numOfBlocks;
}

bool doRevisedResultsByLimit(SQInfo *pQInfo) {
  SQuery *pQuery = &pQInfo->query;

  if ((pQuery->limit.limit > 0) && (pQuery->pointsRead + pQInfo->pointsRead > pQuery->limit.limit)) {
    pQuery->pointsRead = pQuery->limit.limit - pQInfo->pointsRead;

    setQueryStatus(pQuery, QUERY_COMPLETED);  // query completed
    return true;
  }

  return false;
}

static void setExecParams(SQuery *pQuery, SQLFunctionCtx *pCtx, int64_t StartQueryTimestamp, void *inputData,
                          char *primaryColumnData, int32_t size, int32_t functionId, SField *pField, bool hasNull,
                          int32_t blockStatus, void *param, int32_t scanFlag);

void        createGroupResultBuf(SQuery *pQuery, SOutputRes *pOneResult, bool isMetricQuery);
static void destroyGroupResultBuf(SOutputRes *pOneOutputRes, int32_t nOutputCols);

static int32_t binarySearchForBlockImpl(SCompBlock *pBlock, int32_t numOfBlocks, TSKEY skey, int32_t order) {
  int32_t firstSlot = 0;
  int32_t lastSlot = numOfBlocks - 1;

  int32_t midSlot = firstSlot;

  while (1) {
    numOfBlocks = lastSlot - firstSlot + 1;
    midSlot = (firstSlot + (numOfBlocks >> 1));

    if (numOfBlocks == 1) break;

    if (skey > pBlock[midSlot].keyLast) {
      if (numOfBlocks == 2) break;
      if ((order == TSQL_SO_DESC) && (skey < pBlock[midSlot + 1].keyFirst)) break;
      firstSlot = midSlot + 1;
    } else if (skey < pBlock[midSlot].keyFirst) {
      if ((order == TSQL_SO_ASC) && (skey > pBlock[midSlot - 1].keyLast)) break;
      lastSlot = midSlot - 1;
    } else {
      break;  // got the slot
    }
  }

  return midSlot;
}

static int32_t binarySearchForBlock(SQuery *pQuery, int64_t key) {
  return binarySearchForBlockImpl(pQuery->pBlock, pQuery->numOfBlocks, key, pQuery->order.order);
}

#if 0
/* unmap previous buffer */
static UNUSED_FUNC int32_t resetMMapWindow(SHeaderFileInfo *pQueryFileInfo) {
  munmap(pQueryFileInfo->pDataFileData, pQueryFileInfo->defaultMappingSize);

  pQueryFileInfo->dtFileMappingOffset = 0;
  pQueryFileInfo->pDataFileData = mmap(NULL, pQueryFileInfo->defaultMappingSize, PROT_READ, MAP_PRIVATE | MAP_POPULATE,
                                       pQueryFileInfo->dataFd, pQueryFileInfo->dtFileMappingOffset);
  if (pQueryFileInfo->pDataFileData == MAP_FAILED) {
    dError("failed to mmaping data file:%s, reason:%s", pQueryFileInfo->dataFilePath, strerror(errno));
    return -1;
  }

  return 0;
}

static int32_t moveMMapWindow(SHeaderFileInfo *pQueryFileInfo, uint64_t offset) {
  uint64_t upperBnd = (pQueryFileInfo->dtFileMappingOffset + pQueryFileInfo->defaultMappingSize - 1);

  /* data that are located in current mmapping window */
  if ((offset >= pQueryFileInfo->dtFileMappingOffset && offset <= upperBnd) &&
      pQueryFileInfo->pDataFileData != MAP_FAILED) {
    // if it mapping failed, try again when it is called.
    return 0;
  }

  /*
   * 1. there is import data that locate farther from the beginning, but with less timestamp, so we need to move the
   * window backwards
   * 2. otherwise, move the mmaping window forward
   */
  upperBnd = (offset / pQueryFileInfo->defaultMappingSize + 1) * pQueryFileInfo->defaultMappingSize - 1;

  /* unmap previous buffer */
  if (pQueryFileInfo->pDataFileData != MAP_FAILED) {
    int32_t ret = munmap(pQueryFileInfo->pDataFileData, pQueryFileInfo->defaultMappingSize);
    pQueryFileInfo->pDataFileData = MAP_FAILED;
    if (ret != 0) {
      dError("failed to unmmaping data file:%s, handle:%d, offset:%ld, reason:%s", pQueryFileInfo->dataFilePath,
             pQueryFileInfo->dataFd, pQueryFileInfo->dtFileMappingOffset, strerror(errno));
      return -1;
    }
  }

  /* mmap from the new position */
  pQueryFileInfo->dtFileMappingOffset = upperBnd - pQueryFileInfo->defaultMappingSize + 1;
  pQueryFileInfo->pDataFileData = mmap(NULL, pQueryFileInfo->defaultMappingSize, PROT_READ, MAP_PRIVATE | MAP_POPULATE,
                                       pQueryFileInfo->dataFd, pQueryFileInfo->dtFileMappingOffset);
  if (pQueryFileInfo->pDataFileData == MAP_FAILED) {
    dError("failed to mmaping data file:%s, handle:%d, offset:%ld, reason:%s", pQueryFileInfo->dataFilePath,
           pQueryFileInfo->dataFd, pQueryFileInfo->dtFileMappingOffset, strerror(errno));
    return -1;
  }

  /* advise kernel the usage of mmaped data */
  if (madvise(pQueryFileInfo->pDataFileData, pQueryFileInfo->defaultMappingSize, MADV_SEQUENTIAL) == -1) {
    dError("failed to advise kernel the usage of data file:%s, handle:%d, reason:%s", pQueryFileInfo->dataFilePath,
           pQueryFileInfo->dataFd, strerror(errno));
  }

  return 0;
}

static int32_t copyDataFromMMapBuffer(int fd, SQInfo *pQInfo, SHeaderFileInfo *pQueryFile, char *buf, uint64_t offset,
                                      int32_t size) {
  assert(size >= 0);

  int32_t ret = moveMMapWindow(pQueryFile, offset);
  dTrace("QInfo:%p finished move to correct position:%ld", pQInfo, taosGetTimestampUs());

  if (pQueryFile->pDataFileData == MAP_FAILED || ret != TSDB_CODE_SUCCESS) {
    dTrace("QInfo:%p move window failed. ret:%d", pQInfo, ret);
    return -1;
  }

  uint64_t upperBnd = pQueryFile->dtFileMappingOffset + pQueryFile->defaultMappingSize - 1;

  /* data are enclosed in current mmap window */
  if (offset + size <= upperBnd) {
    uint64_t startPos = offset - pQueryFile->dtFileMappingOffset;
    memcpy(buf, pQueryFile->pDataFileData + startPos, size);

    dTrace("QInfo:%p copy data completed, size:%d, time:%ld", pQInfo, size, taosGetTimestampUs());

  } else {
    uint32_t firstPart = upperBnd - offset + 1;
    memcpy(buf, pQueryFile->pDataFileData + (offset - pQueryFile->dtFileMappingOffset), firstPart);

    dTrace("QInfo:%p copy data first part,size:%d, time:%ld", pQInfo, firstPart, taosGetTimestampUs());

    char *dst = buf + firstPart;

    /* remain data */
    uint32_t remain = size - firstPart;
    while (remain > 0) {
      int32_t ret1 = moveMMapWindow(pQueryFile, pQueryFile->dtFileMappingOffset + pQueryFile->defaultMappingSize);
      if (ret1 != 0) {
        return ret1;
      }

      uint32_t len = (remain > pQueryFile->defaultMappingSize) ? pQueryFile->defaultMappingSize : remain;

      /* start from the 0 position */
      memcpy(dst, pQueryFile->pDataFileData, len);
      remain -= len;
      dst += len;

      dTrace("QInfo:%p copy data part,size:%d, time:%ld", pQInfo, len, taosGetTimestampUs());
    }
  }

  return 0;
}

#endif

static int32_t readDataFromDiskFile(int fd, SQInfo *pQInfo, SQueryFilesInfo *pQueryFile, char *buf, uint64_t offset,
                                    int32_t size) {
  assert(size >= 0);

  int32_t ret = (int32_t)lseek(fd, offset, SEEK_SET);
  if (ret == -1) {
    //        qTrace("QInfo:%p seek failed, reason:%s", pQInfo, strerror(errno));
    return -1;
  }

  ret = read(fd, buf, size);
  //    qTrace("QInfo:%p read data %d completed", pQInfo, size);
  return 0;
}

static int32_t loadColumnIntoMem(SQuery *pQuery, SQueryFilesInfo *pQueryFileInfo, SCompBlock *pBlock, SField *pFields,
                                 int32_t col, SData *sdata, void *tmpBuf, char *buffer, int32_t buffersize) {
  char *dst = (pBlock->algorithm) ? tmpBuf : sdata->data;

  int64_t offset = pBlock->offset + pFields[col].offset;
  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  int     fd = pBlock->last ? pQueryFileInfo->lastFd : pQueryFileInfo->dataFd;
  int32_t ret = readDataFromDiskFile(fd, pQInfo, pQueryFileInfo, dst, offset, pFields[col].len);
  if (ret != 0) {
    return ret;
  }

  // load checksum
  TSCKSUM checksum = 0;
  ret = readDataFromDiskFile(fd, pQInfo, pQueryFileInfo, (char *)&checksum, offset + pFields[col].len, sizeof(TSCKSUM));
  if (ret != 0) {
    return ret;
  }

  // check column data integrity
  if (checksum != taosCalcChecksum(0, (const uint8_t *)dst, pFields[col].len)) {
    dLError("QInfo:%p, column data checksum error, file:%s, col: %d, offset:%" PRId64, GET_QINFO_ADDR(pQuery),
            pQueryFileInfo->dataFilePath, col, offset);

    return -1;
  }

  if (pBlock->algorithm) {
    (*pDecompFunc[pFields[col].type])(tmpBuf, pFields[col].len, pBlock->numOfPoints, sdata->data,
                                      pFields[col].bytes * pBlock->numOfPoints, pBlock->algorithm, buffer, buffersize);
  }

  return 0;
}

static int32_t loadDataBlockFieldsInfo(SQueryRuntimeEnv *pRuntimeEnv, SCompBlock *pBlock, SField **pField) {
  SQuery *         pQuery = pRuntimeEnv->pQuery;
  SQInfo *         pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);
  SMeterObj *      pMeterObj = pRuntimeEnv->pMeterObj;
  SQueryFilesInfo *pVnodeFilesInfo = &pRuntimeEnv->vnodeFileInfo;

  size_t size = sizeof(SField) * (pBlock->numOfCols) + sizeof(TSCKSUM);

  // if *pField != NULL, this block is loaded once, in current query do nothing
  if (*pField == NULL) {  // load the fields information once
    *pField = malloc(size);
  }

  SQueryCostSummary *pSummary = &pRuntimeEnv->summary;
  pSummary->totalFieldSize += size;
  pSummary->readField++;
  pSummary->numOfSeek++;

  int64_t st = taosGetTimestampUs();

  int     fd = pBlock->last ? pVnodeFilesInfo->lastFd : pVnodeFilesInfo->dataFd;
  int32_t ret = readDataFromDiskFile(fd, pQInfo, pVnodeFilesInfo, (char *)(*pField), pBlock->offset, size);
  if (ret != 0) {
    return ret;
  }

  // check fields integrity
  if (!taosCheckChecksumWhole((uint8_t *)(*pField), size)) {
    dLError("QInfo:%p vid:%d sid:%d id:%s, slot:%d, failed to read sfields, file:%s, sfields area broken:%" PRId64,
            pQInfo, pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->slot, pVnodeFilesInfo->dataFilePath,
            pBlock->offset);
    return -1;
  }

  int64_t et = taosGetTimestampUs();
  qTrace("QInfo:%p vid:%d sid:%d id:%s, slot:%d, load field info, size:%d, elapsed:%f ms", pQInfo, pMeterObj->vnode,
         pMeterObj->sid, pMeterObj->meterId, pQuery->slot, size, (et - st) / 1000.0);

  pSummary->loadFieldUs += (et - st);
  return 0;
}

static void fillWithNull(SQuery *pQuery, char *dst, int32_t col, int32_t numOfPoints) {
  int32_t bytes = pQuery->colList[col].data.bytes;
  int32_t type = pQuery->colList[col].data.type;

  setNullN(dst, type, bytes, numOfPoints);
}

static int32_t loadPrimaryTSColumn(SQueryRuntimeEnv *pRuntimeEnv, SCompBlock *pBlock, SField **pField,
                                   int32_t *columnBytes) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  assert(PRIMARY_TSCOL_LOADED(pQuery) == false);

  if (columnBytes != NULL) {
    (*columnBytes) += (*pField)[PRIMARYKEY_TIMESTAMP_COL_INDEX].len + sizeof(TSCKSUM);
  }

  int32_t ret = loadColumnIntoMem(pQuery, &pRuntimeEnv->vnodeFileInfo, pBlock, *pField, PRIMARYKEY_TIMESTAMP_COL_INDEX,
                                  pRuntimeEnv->primaryColBuffer, pRuntimeEnv->unzipBuffer,
                                  pRuntimeEnv->secondaryUnzipBuffer, pRuntimeEnv->unzipBufSize);
  return ret;
}

static int32_t loadDataBlockIntoMem(SCompBlock *pBlock, SField **pField, SQueryRuntimeEnv *pRuntimeEnv, int32_t fileIdx,
                                    bool loadPrimaryCol, bool loadSField) {
  int32_t i = 0, j = 0;

  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;
  SData **   sdata = pRuntimeEnv->colDataBuffer;

  assert(fileIdx == pRuntimeEnv->vnodeFileInfo.current);

  SData **primaryTSBuf = &pRuntimeEnv->primaryColBuffer;
  void *  tmpBuf = pRuntimeEnv->unzipBuffer;
  int32_t columnBytes = 0;

  SQueryCostSummary *pSummary = &pRuntimeEnv->summary;

  int32_t status = vnodeIsDatablockLoaded(pRuntimeEnv, pMeterObj, fileIdx, loadPrimaryCol);
  if (status == DISK_BLOCK_NO_NEED_TO_LOAD) {
    dTrace(
        "QInfo:%p vid:%d sid:%d id:%s, fileId:%d, data block has been loaded, no need to load again, ts:%d, slot:%d,"
        " brange:%lld-%lld, rows:%d",
        GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, loadPrimaryCol,
        pQuery->slot, pBlock->keyFirst, pBlock->keyLast, pBlock->numOfPoints);

    if (loadSField && (pQuery->pFields == NULL || pQuery->pFields[pQuery->slot] == NULL)) {
      loadDataBlockFieldsInfo(pRuntimeEnv, pBlock, &pQuery->pFields[pQuery->slot]);
    }

    return TSDB_CODE_SUCCESS;
  } else if (status == DISK_BLOCK_LOAD_TS) {
    dTrace("QInfo:%p vid:%d sid:%d id:%s, fileId:%d, data block has been loaded, incrementally load ts",
           GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId);

    assert(PRIMARY_TSCOL_LOADED(pQuery) == false && loadSField == true);
    if (pQuery->pFields == NULL || pQuery->pFields[pQuery->slot] == NULL) {
      loadDataBlockFieldsInfo(pRuntimeEnv, pBlock, &pQuery->pFields[pQuery->slot]);
    }

    // load primary timestamp
    int32_t ret = loadPrimaryTSColumn(pRuntimeEnv, pBlock, pField, &columnBytes);

    vnodeSetDataBlockInfoLoaded(pRuntimeEnv, pMeterObj, fileIdx, loadPrimaryCol);
    return ret;
  }

  /* failed to load fields info, return with error info */
  if (loadSField && (loadDataBlockFieldsInfo(pRuntimeEnv, pBlock, pField) != 0)) {
    return -1;
  }

  int64_t st = taosGetTimestampUs();

  if (loadPrimaryCol) {
    if (PRIMARY_TSCOL_LOADED(pQuery)) {
      *primaryTSBuf = sdata[0];
    } else {
      int32_t ret = loadPrimaryTSColumn(pRuntimeEnv, pBlock, pField, &columnBytes);
      if (ret != TSDB_CODE_SUCCESS) {
        return ret;
      }

      pSummary->numOfSeek++;
      j += 1;  // first column of timestamp is not needed to be read again
    }
  }

  int32_t ret = 0;

  /* the first round always be 1, the secondary round is determined by queried function */
  int32_t round = pRuntimeEnv->scanFlag;

  while (j < pBlock->numOfCols && i < pQuery->numOfCols) {
    if ((*pField)[j].colId < pQuery->colList[i].data.colId) {
      ++j;
    } else if ((*pField)[j].colId == pQuery->colList[i].data.colId) {
      // add additional check for data type
      if ((*pField)[j].type != pQuery->colList[i].data.type) {
        ret = TSDB_CODE_INVALID_QUERY_MSG;
        break;
      }

      /*
       * during supplementary scan:
       * 1. primary ts column (always loaded)
       * 2. query specified columns
       * 3. in case of filter column required, filter columns must be loaded.
       */
      if (pQuery->colList[i].req[round] == 1 || pQuery->colList[i].data.colId == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
        // if data of this column in current block are all null, do NOT read it from disk
        if ((*pField)[j].numOfNullPoints == pBlock->numOfPoints) {
          fillWithNull(pQuery, sdata[i]->data, i, pBlock->numOfPoints);
        } else {
          columnBytes += (*pField)[j].len + sizeof(TSCKSUM);
          ret = loadColumnIntoMem(pQuery, &pRuntimeEnv->vnodeFileInfo, pBlock, *pField, j, sdata[i], tmpBuf,
                                  pRuntimeEnv->secondaryUnzipBuffer, pRuntimeEnv->unzipBufSize);

          pSummary->numOfSeek++;
        }
      }
      ++i;
      ++j;
    } else {
      /*
       * pQuery->colList[i].colIdx < (*pFields)[j].colId this column is not existed in current block,
       * fill with NULL value
       */
      fillWithNull(pQuery, sdata[i]->data, i, pBlock->numOfPoints);

      pSummary->totalGenData += (pBlock->numOfPoints * pQuery->colList[i].data.bytes);
      ++i;
    }
  }

  if (j >= pBlock->numOfCols && i < pQuery->numOfCols) {
    // remain columns need to set null value
    while (i < pQuery->numOfCols) {
      fillWithNull(pQuery, sdata[i]->data, i, pBlock->numOfPoints);

      pSummary->totalGenData += (pBlock->numOfPoints * pQuery->colList[i].data.bytes);
      ++i;
    }
  }

  int64_t et = taosGetTimestampUs();
  qTrace("QInfo:%p vid:%d sid:%d id:%s, slot:%d, load block completed, ts loaded:%d, rec:%d, elapsed:%f ms",
         GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->slot, loadPrimaryCol,
         pBlock->numOfPoints, (et - st) / 1000.0);

  pSummary->totalBlockSize += columnBytes;
  pSummary->loadBlocksUs += (et - st);
  pSummary->readDiskBlocks++;

  vnodeSetDataBlockInfoLoaded(pRuntimeEnv, pMeterObj, fileIdx, loadPrimaryCol);
  return ret;
}

// todo ignore the blockType, pass the pQuery into this function
SBlockInfo getBlockBasicInfo(SQueryRuntimeEnv *pRuntimeEnv, void *pBlock, int32_t blockType) {
  SBlockInfo blockInfo = {0};
  if (IS_FILE_BLOCK(blockType)) {
    SCompBlock *pDiskBlock = (SCompBlock *)pBlock;

    blockInfo.keyFirst = pDiskBlock->keyFirst;
    blockInfo.keyLast = pDiskBlock->keyLast;
    blockInfo.size = pDiskBlock->numOfPoints;
    blockInfo.numOfCols = pDiskBlock->numOfCols;
  } else {
    SCacheBlock *pCacheBlock = (SCacheBlock *)pBlock;

    blockInfo.keyFirst = getTimestampInCacheBlock(pRuntimeEnv, pCacheBlock, 0);
    blockInfo.keyLast = getTimestampInCacheBlock(pRuntimeEnv, pCacheBlock, pCacheBlock->numOfPoints - 1);
    blockInfo.size = pCacheBlock->numOfPoints;
    blockInfo.numOfCols = pCacheBlock->pMeterObj->numOfColumns;
  }

  return blockInfo;
}

static bool checkQueryRangeAgainstNextBlock(SBlockInfo *pBlockInfo, SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  if ((QUERY_IS_ASC_QUERY(pQuery) && pBlockInfo->keyFirst > pQuery->ekey) ||
      (!QUERY_IS_ASC_QUERY(pQuery) && pBlockInfo->keyLast < pQuery->ekey)) {
    int32_t pos = QUERY_IS_ASC_QUERY(pQuery) ? 0 : pBlockInfo->size - 1;

    savePointPosition(&pRuntimeEnv->nextPos, pQuery->fileId, pQuery->slot, pos);
    setQueryStatus(pQuery, QUERY_COMPLETED);

    return false;
  }

  return true;
}

/**
 *
 * @param pQuery
 * @param pBlockInfo
 * @param forwardStep
 * @return  TRUE means query not completed, FALSE means query is completed
 */
static bool queryCompleteInBlock(SQuery *pQuery, SBlockInfo *pBlockInfo, int32_t forwardStep) {
  if (Q_STATUS_EQUAL(pQuery->over, QUERY_RESBUF_FULL)) {
    //    assert(pQuery->checkBufferInLoop == 1 && pQuery->over == QUERY_RESBUF_FULL && pQuery->pointsOffset == 0);

    assert((QUERY_IS_ASC_QUERY(pQuery) && forwardStep + pQuery->pos <= pBlockInfo->size) ||
           (!QUERY_IS_ASC_QUERY(pQuery) && pQuery->pos - forwardStep + 1 >= 0));

    // current query completed
    if ((pQuery->lastKey > pQuery->ekey && QUERY_IS_ASC_QUERY(pQuery)) ||
        (pQuery->lastKey < pQuery->ekey && !QUERY_IS_ASC_QUERY(pQuery))) {
      setQueryStatus(pQuery, QUERY_COMPLETED);
    }

    return true;
  } else {  // query completed
    if ((pQuery->ekey <= pBlockInfo->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
        (pQuery->ekey >= pBlockInfo->keyFirst && !QUERY_IS_ASC_QUERY(pQuery))) {
      setQueryStatus(pQuery, QUERY_COMPLETED);
      return true;
    }

    return false;
  }
}

/**
 * save triple tuple of (fileId, slot, pos) to SPositionInfo
 */
void savePointPosition(SPositionInfo *position, int32_t fileId, int32_t slot, int32_t pos) {
  /*
   * slot == -1 && pos == -1 means no data left anymore
   */
  assert(fileId >= -1 && slot >= -1 && pos >= -1);

  position->fileId = fileId;
  position->slot = slot;
  position->pos = pos;
}

bool isCacheBlockValid(SQuery *pQuery, SCacheBlock *pBlock, SMeterObj *pMeterObj) {
  if (pMeterObj != pBlock->pMeterObj || pBlock->blockId > pQuery->blockId) {
    SMeterObj *pNewMeterObj = pBlock->pMeterObj;
    char *     id = (pNewMeterObj != NULL) ? pNewMeterObj->meterId : NULL;

    dWarn(
        "QInfo:%p vid:%d sid:%d id:%s, cache block is overwritten, slot:%d blockId:%d qBlockId:%d, meterObj:%p, "
        "blockMeterObj:%p, blockMeter id:%s, first:%d, last:%d, numOfBlocks:%d",
        GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->slot, pBlock->blockId,
        pQuery->blockId, pMeterObj, pNewMeterObj, id, pQuery->firstSlot, pQuery->currentSlot, pQuery->numOfBlocks);

    return false;
  }

  /*
   * The check for empty block:
   *    pBlock->numOfPoints == 0. There is a empty block, which is caused by allocate-and-write data into cache
   *    procedure. The block has been allocated but data has not been put into yet. If the block is the last
   *    block(newly allocated block), abort query. Otherwise, skip it and go on.
   */
  if (pBlock->numOfPoints == 0) {
    dWarn(
        "QInfo:%p vid:%d sid:%d id:%s, cache block is empty. slot:%d first:%d, last:%d, numOfBlocks:%d,"
        "allocated but not write data yet.",
        GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->slot, pQuery->firstSlot,
        pQuery->currentSlot, pQuery->numOfBlocks);

    return false;
  }

  return true;
}

// todo all functions that call this function should check the returned data blocks status
SCacheBlock *getCacheDataBlock(SMeterObj *pMeterObj, SQueryRuntimeEnv *pRuntimeEnv, int32_t slot) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
  if (pCacheInfo == NULL || pCacheInfo->cacheBlocks == NULL || slot < 0 || slot >= pCacheInfo->maxBlocks) {
    return NULL;
  }

  getBasicCacheInfoSnapshot(pQuery, pCacheInfo, pMeterObj->vnode);

  SCacheBlock *pBlock = pCacheInfo->cacheBlocks[slot];
  if (pBlock == NULL) {  // the cache info snapshot must be existed.
    int32_t curNumOfBlocks = pCacheInfo->numOfBlocks;
    int32_t curSlot = pCacheInfo->currentSlot;

    dError(
        "QInfo:%p NULL Block In Cache, snapshot (available blocks:%d, last block:%d), current (available blocks:%d, "
        "last block:%d), accessed null block:%d, pBlockId:%d",
        GET_QINFO_ADDR(pQuery), pQuery->numOfBlocks, pQuery->currentSlot, curNumOfBlocks, curSlot, slot,
        pQuery->blockId);

    return NULL;
  }

  // block is empty or block does not belongs to current table, return NULL value
  if (!isCacheBlockValid(pQuery, pBlock, pMeterObj)) {
    return NULL;
  }

  // the accessed cache block has been loaded already, return directly
  if (vnodeIsDatablockLoaded(pRuntimeEnv, pMeterObj, -1, true) == DISK_BLOCK_NO_NEED_TO_LOAD) {
    TSKEY skey = getTimestampInCacheBlock(pRuntimeEnv, pBlock, 0);
    TSKEY ekey = getTimestampInCacheBlock(pRuntimeEnv, pBlock, pBlock->numOfPoints - 1);

    dTrace(
        "QInfo:%p vid:%d sid:%d id:%s, fileId:%d, cache block has been loaded, no need to load again, ts:%d, "
        "slot:%d, brange:%lld-%lld, rows:%d",
        GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, 1, pQuery->slot,
        skey, ekey, pBlock->numOfPoints);

    return &pRuntimeEnv->cacheBlock;
  }

  // keep the structure as well as the block data into local buffer
  memcpy(&pRuntimeEnv->cacheBlock, pBlock, sizeof(SCacheBlock));

  SCacheBlock *pNewBlock = &pRuntimeEnv->cacheBlock;

  // the commit data points will be ignored
  int32_t offset = 0;
  int32_t numOfPoints = pNewBlock->numOfPoints;
  if (pQuery->firstSlot == pQuery->commitSlot) {
    assert(pQuery->commitPoint >= 0 && pQuery->commitPoint <= pNewBlock->numOfPoints);

    offset = pQuery->commitPoint;
    numOfPoints = pNewBlock->numOfPoints - offset;

    if (offset != 0) {
      dTrace(
          "%p ignore the data in cache block that are commit already, numOfblock:%d slot:%d ignore points:%d. "
          "first:%d last:%d",
          GET_QINFO_ADDR(pQuery), pQuery->numOfBlocks, pQuery->slot, pQuery->commitPoint, pQuery->firstSlot,
          pQuery->currentSlot);
    }

    pNewBlock->numOfPoints = numOfPoints;

    // current block are all commit already, ignore it
    if (pNewBlock->numOfPoints == 0) {
      dTrace(
          "%p ignore current in cache block that are all commit already, numOfblock:%d slot:%d"
          "first:%d last:%d",
          GET_QINFO_ADDR(pQuery), pQuery->numOfBlocks, pQuery->slot, pQuery->firstSlot, pQuery->currentSlot);
      return NULL;
    }
  }

  // keep the data from in cache into the temporarily allocated buffer
  for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
    SColumnInfoEx *pColumnInfoEx = &pQuery->colList[i];

    int16_t columnIndex = pColumnInfoEx->colIdx;
    int16_t columnIndexInBuf = pColumnInfoEx->colIdxInBuf;

    SColumn *pCol = &pMeterObj->schema[columnIndex];

    int16_t bytes = pCol->bytes;
    int16_t type = pCol->type;

    char *dst = pRuntimeEnv->colDataBuffer[columnIndexInBuf]->data;

    if (pQuery->colList[i].colIdx != -1) {
      assert(pCol->colId == pQuery->colList[i].data.colId && bytes == pColumnInfoEx->data.bytes &&
             type == pColumnInfoEx->data.type);

      memcpy(dst, pBlock->offset[columnIndex] + offset * bytes, numOfPoints * bytes);
    } else {
      setNullN(dst, type, bytes, numOfPoints);
    }
  }

  assert(numOfPoints == pNewBlock->numOfPoints);

  // if the primary timestamp are not loaded by default, always load it here into buffer
  if (!PRIMARY_TSCOL_LOADED(pQuery)) {
    memcpy(pRuntimeEnv->primaryColBuffer->data, pBlock->offset[0] + offset * TSDB_KEYSIZE, TSDB_KEYSIZE * numOfPoints);
  }

  pQuery->fileId = -1;
  pQuery->slot = slot;

  if (!isCacheBlockValid(pQuery, pNewBlock, pMeterObj)) {
    return NULL;
  }

  /*
   * the accessed cache block still belongs to current meterObj, go on
   * update the load data block info
   */
  vnodeSetDataBlockInfoLoaded(pRuntimeEnv, pMeterObj, -1, true);

  TSKEY skey = getTimestampInCacheBlock(pRuntimeEnv, pNewBlock, 0);
  TSKEY ekey = getTimestampInCacheBlock(pRuntimeEnv, pNewBlock, numOfPoints - 1);

  dTrace("QInfo:%p vid:%d sid:%d id:%s, fileId:%d, load cache block, ts:%d, slot:%d, brange:%lld-%lld, rows:%d",
         GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, 1, pQuery->slot,
         skey, ekey, numOfPoints);

  return pNewBlock;
}

static SCompBlock *getDiskDataBlock(SQuery *pQuery, int32_t slot) {
  assert(pQuery->fileId >= 0 && slot >= 0 && slot < pQuery->numOfBlocks && pQuery->pBlock != NULL);
  return &pQuery->pBlock[slot];
}

static void *getGenericDataBlock(SMeterObj *pMeterObj, SQueryRuntimeEnv *pRuntimeEnv, int32_t slot) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  if (IS_DISK_DATA_BLOCK(pQuery)) {
    return getDiskDataBlock(pQuery, slot);
  } else {
    return getCacheDataBlock(pMeterObj, pRuntimeEnv, slot);
  }
}

static int32_t getFileIdFromKey(int32_t vid, TSKEY key) {
  SVnodeObj *pVnode = &vnodeList[vid];
  int64_t    delta = (int64_t)pVnode->cfg.daysPerFile * tsMsPerDay[(uint8_t)pVnode->cfg.precision];

  return (int32_t)(key / delta);  // set the starting fileId
}

enum {
  QUERY_RANGE_LESS_EQUAL = 0,
  QUERY_RANGE_GREATER_EQUAL = 1,
};

static bool getQualifiedDataBlock(SMeterObj *pMeterObj, SQueryRuntimeEnv *pRuntimeEnv, int32_t type,
                                  __block_search_fn_t searchFn) {
  int32_t blkIdx = -1;
  int32_t fid = -1;
  int32_t step = (type == QUERY_RANGE_GREATER_EQUAL) ? QUERY_ASC_FORWARD_STEP : QUERY_DESC_FORWARD_STEP;

  SQuery *pQuery = pRuntimeEnv->pQuery;
  pQuery->slot = -1;

  TSKEY key = pQuery->lastKey;

  SData *primaryColBuffer = pRuntimeEnv->primaryColBuffer;
  pQuery->fileId = getFileIdFromKey(pMeterObj->vnode, key) - step;

  while (1) {
    if ((fid = getNextDataFileCompInfo(pRuntimeEnv, pMeterObj, step)) < 0) {
      break;
    }

    blkIdx = binarySearchForBlock(pQuery, key);

    if (type == QUERY_RANGE_GREATER_EQUAL) {
      if (key <= pQuery->pBlock[blkIdx].keyLast) {
        break;
      } else {
        blkIdx = -1;
      }
    } else {
      if (key >= pQuery->pBlock[blkIdx].keyFirst) {
        break;
      } else {
        blkIdx = -1;
      }
    }
  }

  /* failed to find qualified point in file, abort */
  if (blkIdx == -1) {
    return false;
  }

  assert(blkIdx >= 0 && blkIdx < pQuery->numOfBlocks);

  // load first data block into memory failed, caused by disk block error
  bool blockLoaded = false;
  while (blkIdx < pQuery->numOfBlocks && blkIdx >= 0) {
    pQuery->slot = blkIdx;
    if (loadDataBlockIntoMem(&pQuery->pBlock[pQuery->slot], &pQuery->pFields[pQuery->slot], pRuntimeEnv, fid, true,
                             true) == 0) {
      SET_DATA_BLOCK_LOADED(pRuntimeEnv->blockStatus);
      blockLoaded = true;
      break;
    }

    dError("QInfo:%p fileId:%d total numOfBlks:%d blockId:%d load into memory failed due to error in disk files",
           GET_QINFO_ADDR(pQuery), pQuery->fileId, pQuery->numOfBlocks, blkIdx);
    blkIdx += step;
  }

  // failed to load data from disk, abort current query
  if (blockLoaded == false) {
    return false;
  }

  SCompBlock *pBlocks = getDiskDataBlock(pQuery, blkIdx);

  // search qualified points in blk, according to primary key (timestamp) column
  pQuery->pos = searchFn(primaryColBuffer->data, pBlocks->numOfPoints, key, pQuery->order.order);
  assert(pQuery->pos >= 0 && pQuery->fileId >= 0 && pQuery->slot >= 0);

  return true;
}

static SField *getFieldInfo(SQuery *pQuery, SBlockInfo *pBlockInfo, SField *pFields, int32_t column) {
  // no SField info exist, or column index larger than the output column, no result.
  if (pFields == NULL || column >= pQuery->numOfOutputCols) {
    return NULL;
  }

  SColIndexEx *pColIndexEx = &pQuery->pSelectExpr[column].pBase.colInfo;

  // for a tag column, no corresponding field info
  if (TSDB_COL_IS_TAG(pColIndexEx->flag)) {
    return NULL;
  }

  /*
   * Choose the right column field info by field id, since the file block may be out of date,
   * which means the newest table schema is not equalled to the schema of this block.
   */
  for (int32_t i = 0; i < pBlockInfo->numOfCols; ++i) {
    if (pColIndexEx->colId == pFields[i].colId) {
      return &pFields[i];
    }
  }

  return NULL;
}

/*
 * not null data in two cases:
 * 1. tags data: isTag == true;
 * 2. data locate in file, numOfNullPoints == 0 or pFields does not needed to be loaded
 */
static bool hasNullVal(SQuery *pQuery, int32_t col, SBlockInfo *pBlockInfo, SField *pFields, bool isDiskFileBlock) {
  bool ret = true;

  if (TSDB_COL_IS_TAG(pQuery->pSelectExpr[col].pBase.colInfo.flag)) {
    ret = false;
  } else if (isDiskFileBlock) {
    if (pFields == NULL) {
      ret = false;
    } else {
      SField *pField = getFieldInfo(pQuery, pBlockInfo, pFields, col);
      if (pField != NULL && pField->numOfNullPoints == 0) {
        ret = false;
      }
    }
  }

  return ret;
}

static char *doGetDataBlocks(SQuery *pQuery, SData **data, int32_t colIdx) {
  assert(colIdx >= 0 && colIdx < pQuery->numOfCols);
  char *pData = data[colIdx]->data;
  return pData;
}

static char *getDataBlocks(SQueryRuntimeEnv *pRuntimeEnv, SArithmeticSupport *sas, int32_t col, int32_t size) {
  SQuery *        pQuery = pRuntimeEnv->pQuery;
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;

  char *dataBlock = NULL;

  int32_t functionId = pQuery->pSelectExpr[col].pBase.functionId;

  if (functionId == TSDB_FUNC_ARITHM) {
    sas->pExpr = &pQuery->pSelectExpr[col];

    // set the start offset to be the lowest start position, no matter asc/desc query order
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      pCtx->startOffset = pQuery->pos;
    } else {
      pCtx->startOffset = pQuery->pos - (size - 1);
    }

    for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
      SColumnInfo *pColMsg = &pQuery->colList[i].data;
      char *       pData = doGetDataBlocks(pQuery, pRuntimeEnv->colDataBuffer, pQuery->colList[i].colIdxInBuf);

      sas->elemSize[i] = pColMsg->bytes;
      sas->data[i] = pData + pCtx->startOffset * sas->elemSize[i];  // start from the offset
    }

    sas->numOfCols = pQuery->numOfCols;
    sas->offset = 0;
  } else {  // other type of query function
    SColIndexEx *pCol = &pQuery->pSelectExpr[col].pBase.colInfo;
    if (TSDB_COL_IS_TAG(pCol->flag)) {
      dataBlock = NULL;
    } else {
      /*
       *  the colIdx is acquired from the first meter of all qualified meters in this vnode during query prepare stage,
       *  the remain meter may not have the required column in cache actually.
       *  So, the validation of required column in cache with the corresponding meter schema is reinforced.
       */
      dataBlock = doGetDataBlocks(pQuery, pRuntimeEnv->colDataBuffer, pCol->colIdxInBuf);
    }
  }

  return dataBlock;
}

/**
 *
 * @param pRuntimeEnv
 * @param forwardStep
 * @param primaryKeyCol
 * @param pFields
 * @param isDiskFileBlock
 * @return                  the incremental number of output value, so it maybe 0 for fixed number of query,
 *                          such as count/min/max etc.
 */
static int32_t blockwiseApplyAllFunctions(SQueryRuntimeEnv *pRuntimeEnv, int32_t forwardStep, TSKEY *primaryKeyCol,
                                          SField *pFields, SBlockInfo *pBlockInfo) {
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;
  SQuery *        pQuery = pRuntimeEnv->pQuery;

  bool    isDiskFileBlock = IS_FILE_BLOCK(pRuntimeEnv->blockStatus);
  int64_t prevNumOfRes = getNumOfResult(pRuntimeEnv);

  SArithmeticSupport *sasArray = calloc((size_t)pQuery->numOfOutputCols, sizeof(SArithmeticSupport));

  for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
    int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;

    SField dummyField = {0};

    bool  hasNull = hasNullVal(pQuery, k, pBlockInfo, pFields, isDiskFileBlock);
    char *dataBlock = getDataBlocks(pRuntimeEnv, &sasArray[k], k, forwardStep);

    SField *tpField = NULL;

    if (pFields != NULL) {
      tpField = getFieldInfo(pQuery, pBlockInfo, pFields, k);
      /*
       * Field info not exist, the required column is not present in current block,
       * so all data must be null value in current block.
       */
      if (tpField == NULL) {
        tpField = &dummyField;
        tpField->numOfNullPoints = (int32_t)forwardStep;
      }
    }

    TSKEY ts = QUERY_IS_ASC_QUERY(pQuery) ? pRuntimeEnv->intervalWindow.skey : pRuntimeEnv->intervalWindow.ekey;
    setExecParams(pQuery, &pCtx[k], ts, dataBlock, (char *)primaryKeyCol, forwardStep, functionId, tpField, hasNull,
                  pRuntimeEnv->blockStatus, &sasArray[k], pRuntimeEnv->scanFlag);
  }

  /*
   * the sqlfunctionCtx parameters should be set done before all functions are invoked,
   * since the selectivity + tag_prj query needs all parameters been set done.
   * tag_prj function are changed to be TSDB_FUNC_TAG_DUMMY
   */
  for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
    int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;
    if (functionNeedToExecute(pRuntimeEnv, &pCtx[k], functionId)) {
      aAggs[functionId].xFunction(&pCtx[k]);
    }
  }

  int64_t numOfIncrementRes = getNumOfResult(pRuntimeEnv) - prevNumOfRes;
  validateTimestampForSupplementResult(pRuntimeEnv, numOfIncrementRes);

  tfree(sasArray);

  return (int32_t)numOfIncrementRes;
}

/**
 * if sfields is null
 * 1. count(*)/spread(ts) is invoked
 * 2. this column does not exists
 *
 * first filter the data block according to the value filter condition, then, if the top/bottom query applied,
 * invoke the filter function to decide if the data block need to be accessed or not.
 * TODO handle the whole data block is NULL situation
 * @param pQuery
 * @param pField
 * @return
 */
static bool needToLoadDataBlock(SQuery *pQuery, SField *pField, SQLFunctionCtx *pCtx, int32_t numOfTotalPoints) {
  if (pField == NULL) {
    return false;  // no need to load data
  }

  for (int32_t k = 0; k < pQuery->numOfFilterCols; ++k) {
    SSingleColumnFilterInfo *pFilterInfo = &pQuery->pFilterInfo[k];
    int32_t                  colIndex = pFilterInfo->info.colIdx;

    // this column not valid in current data block
    if (colIndex < 0 || pField[colIndex].colId != pFilterInfo->info.data.colId) {
      continue;
    }

    // not support pre-filter operation on binary/nchar data type
    if (!vnodeSupportPrefilter(pFilterInfo->info.data.type)) {
      continue;
    }

    // all points in current column are NULL, no need to check its boundary value
    if (pField[colIndex].numOfNullPoints == numOfTotalPoints) {
      continue;
    }

    if (pFilterInfo->info.data.type == TSDB_DATA_TYPE_FLOAT) {
      float minval = *(double *)(&pField[colIndex].min);
      float maxval = *(double *)(&pField[colIndex].max);

      for (int32_t i = 0; i < pFilterInfo->numOfFilters; ++i) {
        if (pFilterInfo->pFilters[i].fp(&pFilterInfo->pFilters[i], (char *)&minval, (char *)&maxval)) {
          return true;
        }
      }
    } else {
      for (int32_t i = 0; i < pFilterInfo->numOfFilters; ++i) {
        if (pFilterInfo->pFilters[i].fp(&pFilterInfo->pFilters[i], (char *)&pField[colIndex].min,
                                        (char *)&pField[colIndex].max)) {
          return true;
        }
      }
    }
  }

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functId == TSDB_FUNC_TOP || functId == TSDB_FUNC_BOTTOM) {
      return top_bot_datablock_filter(&pCtx[i], functId, (char *)&pField[i].min, (char *)&pField[i].max);
    }
  }

  return true;
}

static SOutputRes *doSetSlidingWindowFromKey(SSlidingWindowInfo *pSlidingWindowInfo, char *pData, int16_t bytes,
                                SWindowStatus **pStatus) {
  int32_t p = -1;

  int32_t *p1 = (int32_t *)taosGetDataFromHash(pSlidingWindowInfo->hashList, pData, bytes);
  if (p1 != NULL) {
    p = *p1;

    pSlidingWindowInfo->curIndex = p;
    if (pStatus != NULL) {
      *pStatus = &pSlidingWindowInfo->pStatus[p];
    }
  } else {  // more than the capacity, reallocate the resources
    if (pSlidingWindowInfo->size >= pSlidingWindowInfo->capacity) {
      int64_t newCap = pSlidingWindowInfo->capacity * 2;

      char *t = realloc(pSlidingWindowInfo->pStatus, newCap * sizeof(SWindowStatus));
      if (t != NULL) {
        pSlidingWindowInfo->pStatus = (SWindowStatus *)t;
        memset(&pSlidingWindowInfo->pStatus[pSlidingWindowInfo->capacity], 0, sizeof(SWindowStatus) * pSlidingWindowInfo->capacity);
      } else {
        // todo
      }

      pSlidingWindowInfo->capacity = newCap;
    }

    // add a new result set for a new group
    if (pStatus != NULL) {
      *pStatus = &pSlidingWindowInfo->pStatus[pSlidingWindowInfo->size];
    }

    p = pSlidingWindowInfo->size;
    pSlidingWindowInfo->curIndex = pSlidingWindowInfo->size;

    pSlidingWindowInfo->size += 1;
    taosAddToHashTable(pSlidingWindowInfo->hashList, pData, bytes, (char *)&pSlidingWindowInfo->curIndex, sizeof(int32_t));
  }

  return &pSlidingWindowInfo->pResult[p];
}

static int32_t initSlidingWindowInfo(SSlidingWindowInfo *pSlidingWindowInfo, int32_t threshold, int16_t type,
                                 SOutputRes *pRes) {
  pSlidingWindowInfo->capacity = threshold;
  pSlidingWindowInfo->threshold = threshold;

  pSlidingWindowInfo->type = type;

  _hash_fn_t fn = taosGetDefaultHashFunction(type);
  pSlidingWindowInfo->hashList = taosInitHashTable(threshold, fn, false);

  pSlidingWindowInfo->curIndex = -1;
  pSlidingWindowInfo->size = 0;
  pSlidingWindowInfo->pResult = pRes;
  pSlidingWindowInfo->pStatus = calloc(threshold, sizeof(SWindowStatus));

  if (pSlidingWindowInfo->pStatus == NULL || pSlidingWindowInfo->hashList == NULL) {
    return -1;
  }

  return TSDB_CODE_SUCCESS;
}

static void destroySlidingWindowInfo(SSlidingWindowInfo *pSlidingWindowInfo) {
  if (pSlidingWindowInfo == NULL || pSlidingWindowInfo->capacity == 0) {
    assert(pSlidingWindowInfo->hashList == NULL && pSlidingWindowInfo->pResult == NULL);
    return;
  }

  taosCleanUpHashTable(pSlidingWindowInfo->hashList);
  tfree(pSlidingWindowInfo->pStatus);
}

void resetSlidingWindowInfo(SSlidingWindowInfo *pSlidingWindowInfo, int32_t numOfCols) {
  if (pSlidingWindowInfo == NULL || pSlidingWindowInfo->capacity == 0) {
    return;
  }

  for (int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
    SOutputRes *pOneRes = &pSlidingWindowInfo->pResult[i];
    clearGroupResultBuf(pOneRes, numOfCols);
  }

  memset(pSlidingWindowInfo->pStatus, 0, sizeof(SWindowStatus) * pSlidingWindowInfo->capacity);

  pSlidingWindowInfo->curIndex = -1;
  taosCleanUpHashTable(pSlidingWindowInfo->hashList);
  pSlidingWindowInfo->size = 0;

  _hash_fn_t fn = taosGetDefaultHashFunction(pSlidingWindowInfo->type);
  pSlidingWindowInfo->hashList = taosInitHashTable(pSlidingWindowInfo->capacity, fn, false);

  pSlidingWindowInfo->startTime = 0;
  pSlidingWindowInfo->prevSKey = 0;
}

void clearCompletedSlidingWindows(SSlidingWindowInfo *pSlidingWindowInfo, int32_t numOfCols) {
  if (pSlidingWindowInfo == NULL || pSlidingWindowInfo->capacity == 0 || pSlidingWindowInfo->size == 0) {
    return;
  }

  int32_t i = 0;
  for (i = 0; i < pSlidingWindowInfo->size; ++i) {
    SWindowStatus *pStatus = &pSlidingWindowInfo->pStatus[i];
    if (pStatus->closed) {  // remove the window slot from hash table
      taosDeleteFromHashTable(pSlidingWindowInfo->hashList, (const char *)&pStatus->window.skey, TSDB_KEYSIZE);
    } else {
      break;
    }
  }

  if (i == 0) {
    return;
  }

  int32_t remain = pSlidingWindowInfo->size - i;
  //clear remain list
  memmove(pSlidingWindowInfo->pStatus, &pSlidingWindowInfo->pStatus[i], remain * sizeof(SWindowStatus));
  memset(&pSlidingWindowInfo->pStatus[remain], 0, (pSlidingWindowInfo->capacity - remain) * sizeof(SWindowStatus));
  
  for(int32_t k = 0; k < remain; ++k) {
    copyGroupResultBuf(&pSlidingWindowInfo->pResult[k], &pSlidingWindowInfo->pResult[i + k], numOfCols);
  }
  
  for(int32_t k = remain; k < pSlidingWindowInfo->size; ++k) {
    SOutputRes *pOneRes = &pSlidingWindowInfo->pResult[k];
    clearGroupResultBuf(pOneRes, numOfCols);
  }

  pSlidingWindowInfo->size = remain;

  for(int32_t k = 0; k < pSlidingWindowInfo->size; ++k) {
    SWindowStatus* pStatus = &pSlidingWindowInfo->pStatus[k];
    int32_t *p = (int32_t*) taosGetDataFromHash(pSlidingWindowInfo->hashList, (const char*)&pStatus->window.skey, TSDB_KEYSIZE);
    int32_t v = *p;
    v = (v - i);
  
    taosDeleteFromHashTable(pSlidingWindowInfo->hashList, (const char *)&pStatus->window.skey, TSDB_KEYSIZE);
  
    taosAddToHashTable(pSlidingWindowInfo->hashList, (const char*)&pStatus->window.skey, TSDB_KEYSIZE,
        (char *)&v, sizeof(int32_t));
  }
  
  pSlidingWindowInfo->curIndex = -1;
}

int32_t numOfClosedSlidingWindow(SSlidingWindowInfo *pSlidingWindowInfo) {
  for (int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
    SWindowStatus *pStatus = &pSlidingWindowInfo->pStatus[i];
    if (pStatus->closed == false) {
      return i;
    }
  }
  
  return 0;
}

void closeSlidingWindow(SSlidingWindowInfo* pSlidingWindowInfo, int32_t slot) {
  assert(slot >= 0 && slot < pSlidingWindowInfo->size);
  SWindowStatus* pStatus = &pSlidingWindowInfo->pStatus[slot];
  pStatus->closed = true;
}

void closeAllSlidingWindow(SSlidingWindowInfo* pSlidingWindowInfo) {
  assert(pSlidingWindowInfo->size >=0 && pSlidingWindowInfo->capacity >= pSlidingWindowInfo->size);
  
  for(int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
    SWindowStatus* pStatus = &pSlidingWindowInfo->pStatus[i];
    pStatus->closed = true;
  }
}

static SWindowStatus* getSlidingWindowStatus(SSlidingWindowInfo *pSlidingWindowInfo, int32_t slot) {
  return &pSlidingWindowInfo->pStatus[slot];
}

static bool slidingWindowClosed(SSlidingWindowInfo* pSlidingWindowInfo, int32_t slot) {
  return (pSlidingWindowInfo->pStatus[slot].closed == true);
}

static int32_t curSlidingWindow(SSlidingWindowInfo *pSlidingWindowInfo) {
  assert(pSlidingWindowInfo->curIndex >= 0 && pSlidingWindowInfo->curIndex < pSlidingWindowInfo->size);
  
  return pSlidingWindowInfo->curIndex;
}

// get the correct sliding window according to the handled timestamp
static STimeWindow getActiveSlidingWindow(SSlidingWindowInfo* pSlidingWindowInfo, int64_t ts, SQuery* pQuery) {
  STimeWindow w = {0};
  
  if (pSlidingWindowInfo->curIndex == -1) { // the first window, from the prevous stored value
    w.skey = pSlidingWindowInfo->prevSKey;
    w.ekey = w.skey + pQuery->nAggTimeInterval - 1;
    
  } else {
    SWindowStatus* pStatus = getSlidingWindowStatus(pSlidingWindowInfo, curSlidingWindow(pSlidingWindowInfo));
    
    if (pStatus->window.skey <= ts && pStatus->window.ekey >= ts) {
      w = pStatus->window;
    } else {
      int64_t st = pStatus->window.skey;
      
      while (st > ts) {
        st -= pQuery->slidingTime;
      }
      
      while ((st + pQuery->nAggTimeInterval - 1) < ts) {
        st += pQuery->slidingTime;
      }
  
      w.skey = st;
      w.ekey = w.skey + pQuery->nAggTimeInterval - 1;
    }
  }
  
  assert(ts >= w.skey && ts <= w.ekey);
  return w;
}

static int32_t setGroupResultFromKey(SQueryRuntimeEnv *pRuntimeEnv, char *pData, int16_t type, int16_t bytes) {
  if (isNull(pData, type)) {  // ignore the null value
    return -1;
  }

  SOutputRes *pOutputRes = doSetSlidingWindowFromKey(&pRuntimeEnv->swindowResInfo, pData, bytes, NULL);
  if (pOutputRes == NULL) {
    return -1;
  }

  setGroupOutputBuffer(pRuntimeEnv, pOutputRes);
  initCtxOutputBuf(pRuntimeEnv);

  return TSDB_CODE_SUCCESS;
}

static int32_t setSlidingWindowFromKey(SQueryRuntimeEnv *pRuntimeEnv, STimeWindow *pTimeWindow) {
  assert(pTimeWindow->skey < pTimeWindow->ekey);
  
  int64_t st = pTimeWindow->skey;
 
  SWindowStatus *pStatus = NULL;
  SOutputRes*   pOutputRes = doSetSlidingWindowFromKey(&pRuntimeEnv->swindowResInfo, (char *)&st, TSDB_KEYSIZE,
      &pStatus);
  
  if (pOutputRes == NULL) {
    return -1;
  }

  pStatus->window = *pTimeWindow;
  setGroupOutputBuffer(pRuntimeEnv, pOutputRes);
  initCtxOutputBuf(pRuntimeEnv);

  return TSDB_CODE_SUCCESS;
}

static char *getGroupbyColumnData(SQuery *pQuery, SData **data, int16_t *type, int16_t *bytes) {
  char *groupbyColumnData = NULL;

  SSqlGroupbyExpr *pGroupbyExpr = pQuery->pGroupbyExpr;

  for (int32_t k = 0; k < pGroupbyExpr->numOfGroupCols; ++k) {
    if (pGroupbyExpr->columnInfo[k].flag == TSDB_COL_TAG) {
      continue;
    }

    int16_t colIndex = -1;
    int32_t colId = pGroupbyExpr->columnInfo[k].colId;

    for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
      if (pQuery->colList[i].data.colId == colId) {
        colIndex = i;
        break;
      }
    }

    assert(colIndex >= 0 && colIndex < pQuery->numOfCols);

    *type = pQuery->colList[colIndex].data.type;
    *bytes = pQuery->colList[colIndex].data.bytes;

    groupbyColumnData = doGetDataBlocks(pQuery, data, pQuery->colList[colIndex].colIdxInBuf);
    break;
  }

  return groupbyColumnData;
}

static int32_t doTSJoinFilter(SQueryRuntimeEnv *pRuntimeEnv, int32_t offset) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  STSElem         elem = tsBufGetElem(pRuntimeEnv->pTSBuf);
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;

  // compare tag first
  if (pCtx[0].tag.i64Key != elem.tag) {
    return TS_JOIN_TAG_NOT_EQUALS;
  }

  TSKEY key = *(TSKEY *)(pCtx[0].aInputElemBuf + TSDB_KEYSIZE * offset);

#if defined(_DEBUG_VIEW)
  printf("elem in comp ts file:%" PRId64 ", key:%" PRId64
         ", tag:%d, id:%s, query order:%d, ts order:%d, traverse:%d, index:%d\n",
         elem.ts, key, elem.tag, pRuntimeEnv->pMeterObj->meterId, pQuery->order.order, pRuntimeEnv->pTSBuf->tsOrder,
         pRuntimeEnv->pTSBuf->cur.order, pRuntimeEnv->pTSBuf->cur.tsIndex);
#endif

  if (QUERY_IS_ASC_QUERY(pQuery)) {
    if (key < elem.ts) {
      return TS_JOIN_TS_NOT_EQUALS;
    } else if (key > elem.ts) {
      assert(false);
    }
  } else {
    if (key > elem.ts) {
      return TS_JOIN_TS_NOT_EQUALS;
    } else if (key < elem.ts) {
      assert(false);
    }
  }

  return TS_JOIN_TS_EQUAL;
}

static bool functionNeedToExecute(SQueryRuntimeEnv *pRuntimeEnv, SQLFunctionCtx *pCtx, int32_t functionId) {
  SResultInfo *pResInfo = GET_RES_INFO(pCtx);

  if (pResInfo->complete || functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
    return false;
  }

  // in the supplementary scan, only the following functions need to be executed
  if (!IS_MASTER_SCAN(pRuntimeEnv) &&
      !(functionId == TSDB_FUNC_LAST_DST || functionId == TSDB_FUNC_FIRST_DST || functionId == TSDB_FUNC_FIRST ||
        functionId == TSDB_FUNC_LAST || functionId == TSDB_FUNC_TAG || functionId == TSDB_FUNC_TS)) {
    return false;
  }

  return true;
}

static int32_t rowwiseApplyAllFunctions(SQueryRuntimeEnv *pRuntimeEnv, int32_t *forwardStep, TSKEY *primaryKeyCol,
                                        SField *pFields, SBlockInfo *pBlockInfo) {
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;
  SQuery *        pQuery = pRuntimeEnv->pQuery;

  bool    isDiskFileBlock = IS_FILE_BLOCK(pRuntimeEnv->blockStatus);
  SData **data = pRuntimeEnv->colDataBuffer;

  int64_t prevNumOfRes = 0;
  bool    groupbyStateValue = isGroupbyNormalCol(pQuery->pGroupbyExpr);

  if (!groupbyStateValue) {
    prevNumOfRes = getNumOfResult(pRuntimeEnv);
  }

  SArithmeticSupport *sasArray = calloc((size_t)pQuery->numOfOutputCols, sizeof(SArithmeticSupport));

  int16_t type = 0;
  int16_t bytes = 0;

  char *groupbyColumnData = NULL;
  if (groupbyStateValue) {
    groupbyColumnData = getGroupbyColumnData(pQuery, data, &type, &bytes);
  }

  for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
    int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;

    bool  hasNull = hasNullVal(pQuery, k, pBlockInfo, pFields, isDiskFileBlock);
    char *dataBlock = getDataBlocks(pRuntimeEnv, &sasArray[k], k, *forwardStep);

    TSKEY ts = QUERY_IS_ASC_QUERY(pQuery) ? pRuntimeEnv->intervalWindow.skey : pRuntimeEnv->intervalWindow.ekey;
    setExecParams(pQuery, &pCtx[k], ts, dataBlock, (char *)primaryKeyCol, (*forwardStep), functionId, pFields, hasNull,
                  pRuntimeEnv->blockStatus, &sasArray[k], pRuntimeEnv->scanFlag);
  }

  // set the input column data
  for (int32_t k = 0; k < pQuery->numOfFilterCols; ++k) {
    SSingleColumnFilterInfo *pFilterInfo = &pQuery->pFilterInfo[k];
    /*
     * NOTE: here the tbname/tags column cannot reach here, since it will never be a filter column,
     * so we do NOT check if is a tag or not
     */
    pFilterInfo->pData = doGetDataBlocks(pQuery, data, pFilterInfo->info.colIdxInBuf);
  }

  int32_t numOfRes = 0;
  int32_t step = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  // from top to bottom in desc
  // from bottom to top in asc order
  if (pRuntimeEnv->pTSBuf != NULL) {
    SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);
    qTrace("QInfo:%p process data rows, numOfRows:%d, query order:%d, ts comp order:%d", pQInfo, *forwardStep,
           pQuery->order.order, pRuntimeEnv->pTSBuf->cur.order);
  }

  int32_t j = 0;
  int64_t lastKey = 0;
  
  for (j = 0; j < (*forwardStep); ++j) {
    int32_t offset = GET_COL_DATA_POS(pQuery, j, step);

    if (pRuntimeEnv->pTSBuf != NULL) {
      int32_t r = doTSJoinFilter(pRuntimeEnv, offset);

      if (r == TS_JOIN_TAG_NOT_EQUALS) {
        break;
      } else if (r == TS_JOIN_TS_NOT_EQUALS) {
        continue;
      } else {
        assert(r == TS_JOIN_TS_EQUAL);
      }
    }

    if (pQuery->numOfFilterCols > 0 && (!vnodeDoFilterData(pQuery, offset))) {
      continue;
    }

    // sliding window query
    if (pQuery->slidingTime > 0 && pQuery->nAggTimeInterval > 0) {
      // decide the time window according to the primary timestamp
      int64_t ts = primaryKeyCol[offset];
      
      SSlidingWindowInfo* pSlidingWindowInfo = &pRuntimeEnv->swindowResInfo;
      STimeWindow win = getActiveSlidingWindow(pSlidingWindowInfo, ts, pQuery);

      int32_t ret = setSlidingWindowFromKey(pRuntimeEnv, &win);
      if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
        continue;
      }

      // all startOffset are identical
      offset -= pCtx[0].startOffset;

      for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
        int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;
        pCtx[k].nStartQueryTimestamp = win.skey;
        
        SWindowStatus* pStatus = getSlidingWindowStatus(pSlidingWindowInfo, curSlidingWindow(pSlidingWindowInfo));
        
        if (!IS_MASTER_SCAN(pRuntimeEnv) && !pStatus->closed) {
//          qTrace("QInfo:%p not completed in supplementary scan, ignore funcId:%d, window:%lld-%lld",
//              GET_QINFO_ADDR(pQuery), functionId, pStatus->window.skey, pStatus->window.ekey);
          continue;
        }
        
        if (functionNeedToExecute(pRuntimeEnv, &pCtx[k], functionId)) {
          aAggs[functionId].xFunctionF(&pCtx[k], offset);
        }
      }

      lastKey = ts;
      int32_t index = pRuntimeEnv->swindowResInfo.curIndex;
      
      STimeWindow nextWin = win;
      while (1) {
        getNextLogicalQueryRange(pRuntimeEnv, &nextWin);
        if (pSlidingWindowInfo->startTime > nextWin.skey || (nextWin.skey > pQuery->ekey && QUERY_IS_ASC_QUERY(pQuery)) ||
            (nextWin.skey > pQuery->skey && !QUERY_IS_ASC_QUERY(pQuery))) {
          pRuntimeEnv->swindowResInfo.curIndex = index;
          break;
        }

        if (ts >= nextWin.skey && ts <= nextWin.ekey) {
            // null data, failed to allocate more memory buffer
          if (setSlidingWindowFromKey(pRuntimeEnv, &nextWin) != TSDB_CODE_SUCCESS) {
            pRuntimeEnv->swindowResInfo.curIndex = index;
            break;
          }

          for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
            int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;
            pCtx[k].nStartQueryTimestamp = nextWin.skey;
  
            SWindowStatus* pStatus = getSlidingWindowStatus(pSlidingWindowInfo, curSlidingWindow(pSlidingWindowInfo));
            if (!IS_MASTER_SCAN(pRuntimeEnv) && !pStatus->closed) {
//              qTrace("QInfo:%p not completed in supplementary scan, ignore funcId:%d, window:%lld-%lld",
//                     GET_QINFO_ADDR(pQuery), functionId, pStatus->window.skey, pStatus->window.ekey);
              continue;
            }
            
            if (functionNeedToExecute(pRuntimeEnv, &pCtx[k], functionId)) {
              aAggs[functionId].xFunctionF(&pCtx[k], offset);
            }
          }
        } else {
          pRuntimeEnv->swindowResInfo.curIndex = index;
          break;
        }
      }
    } else {  // other queries
      // decide which group this rows belongs to according to current state value
      if (groupbyStateValue) {
        char *stateVal = groupbyColumnData + bytes * offset;

        int32_t ret = setGroupResultFromKey(pRuntimeEnv, stateVal, type, bytes);
        if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
          continue;
        }
      }

      // all startOffset are identical
      offset -= pCtx[0].startOffset;

      for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
        int32_t functionId = pQuery->pSelectExpr[k].pBase.functionId;
        if (functionNeedToExecute(pRuntimeEnv, &pCtx[k], functionId)) {
          aAggs[functionId].xFunctionF(&pCtx[k], offset);
        }
      }
    }

    if (pRuntimeEnv->pTSBuf != NULL) {
      // if timestamp filter list is empty, quit current query
      if (!tsBufNextPos(pRuntimeEnv->pTSBuf)) {
        setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
        break;
      }
    }

    /*
     * pointsOffset is the maximum available space in result buffer update the actual forward step for query that
     * requires checking buffer during loop
     */
    if ((pQuery->checkBufferInLoop == 1) && (++numOfRes) >= pQuery->pointsOffset) {
      pQuery->lastKey = primaryKeyCol[pQuery->pos + j * step] + step;
      *forwardStep = j + 1;
      break;
    }
  }

  free(sasArray);

  if (pQuery->slidingTime > 0 && pQuery->nAggTimeInterval > 0 && IS_MASTER_SCAN(pRuntimeEnv)) {
    SSlidingWindowInfo *pSlidingWindowInfo = &pRuntimeEnv->swindowResInfo;

    // query completed
    if ((lastKey >= pQuery->ekey && QUERY_IS_ASC_QUERY(pQuery)) ||
        (lastKey <= pQuery->ekey && !QUERY_IS_ASC_QUERY(pQuery))) {
      closeAllSlidingWindow(pSlidingWindowInfo);
      
      pSlidingWindowInfo->curIndex = pSlidingWindowInfo->size - 1;
      setQueryStatus(pQuery, QUERY_COMPLETED | QUERY_RESBUF_FULL);
    } else {
      int32_t i = 0;
      int64_t skey = 0;

      for (i = 0; i < pSlidingWindowInfo->size; ++i) {
        SWindowStatus *pStatus = &pSlidingWindowInfo->pStatus[i];
        if ((pStatus->window.ekey <= lastKey && QUERY_IS_ASC_QUERY(pQuery)) ||
            (pStatus->window.skey >= lastKey && !QUERY_IS_ASC_QUERY(pQuery))) {
          closeSlidingWindow(pSlidingWindowInfo, i);
        } else {
          skey = pStatus->window.skey;
          break;
        }
      }

      pSlidingWindowInfo->prevSKey = skey;
      
      // the number of completed slots are larger than the threshold, dump to client immediately.
      int32_t v = numOfClosedSlidingWindow(pSlidingWindowInfo);
      if (v > pSlidingWindowInfo->threshold) {
        setQueryStatus(pQuery, QUERY_RESBUF_FULL);
      }
      
      dTrace("QInfo:%p total window:%d, closed:%d", GET_QINFO_ADDR(pQuery), pSlidingWindowInfo->size, v);
    }
  }

  /*
   * No need to calculate the number of output results for groupby normal columns
   * because the results of group by normal column is put into intermediate buffer.
   */
  int32_t num = 0;
  if (!groupbyStateValue && !(pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    num = getNumOfResult(pRuntimeEnv) - prevNumOfRes;
  }

  return num;
}

static int32_t getForwardStepsInBlock(int32_t numOfPoints, __block_search_fn_t searchFn, SQuery *pQuery,
                                      int64_t *pData) {
  int32_t endPos = searchFn((char *)pData, numOfPoints, pQuery->ekey, pQuery->order.order);
  int32_t forwardStep = 0;

  if (endPos >= 0) {
    forwardStep = QUERY_IS_ASC_QUERY(pQuery) ? (endPos - pQuery->pos) : (pQuery->pos - endPos);
    assert(forwardStep >= 0);

    // endPos data is equalled to the key so, we do need to read the element in endPos
    if (pData[endPos] == pQuery->ekey) {
      forwardStep += 1;
    }
  }
  return forwardStep;
}

static int32_t reviseForwardSteps(SQueryRuntimeEnv *pRuntimeEnv, int32_t forwardStep) {
  /*
   * 1. If value filter exists, we try all data in current block, and do not set the QUERY_RESBUF_FULL flag.
   *
   * 2. In case of top/bottom/ts_comp query, the checkBufferInLoop == 1 and pQuery->numOfFilterCols
   * may be 0 or not. We do not check the capacity of output buffer, since the filter function will do it.
   *
   * 3. In handling the query of secondary query of join, tsBuf servers as a ts filter.
   */
  SQuery *pQuery = pRuntimeEnv->pQuery;

  if (isTopBottomQuery(pQuery) || isTSCompQuery(pQuery) || pQuery->numOfFilterCols > 0 || pRuntimeEnv->pTSBuf != NULL) {
    return forwardStep;
  }

  // current buffer does not have enough space, try in the next loop
  if ((pQuery->checkBufferInLoop == 1) && (pQuery->pointsOffset <= forwardStep)) {
    forwardStep = pQuery->pointsOffset;
  }

  return forwardStep;
}

static void validateQueryRangeAndData(SQueryRuntimeEnv *pRuntimeEnv, const TSKEY *pPrimaryColumn,
                                      SBlockInfo *pBlockBasicInfo) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  TSKEY startKey = -1;
  // timestamp qualification check
  if (IS_DATA_BLOCK_LOADED(pRuntimeEnv->blockStatus) && needPrimaryTimestampCol(pQuery, pBlockBasicInfo)) {
    startKey = pPrimaryColumn[pQuery->pos];
  } else {
    startKey = pBlockBasicInfo->keyFirst;
    TSKEY endKey = pBlockBasicInfo->keyLast;

    assert((endKey <= pQuery->ekey && QUERY_IS_ASC_QUERY(pQuery)) ||
           (endKey >= pQuery->ekey && !QUERY_IS_ASC_QUERY(pQuery)));
  }

  assert((startKey >= pQuery->lastKey && startKey <= pQuery->ekey && pQuery->skey <= pQuery->lastKey &&
          QUERY_IS_ASC_QUERY(pQuery)) ||
         (startKey <= pQuery->lastKey && startKey >= pQuery->ekey && pQuery->skey >= pQuery->lastKey &&
          !QUERY_IS_ASC_QUERY(pQuery)));
}

static int32_t applyFunctionsOnBlock(SQueryRuntimeEnv *pRuntimeEnv, SBlockInfo *pBlockInfo, int64_t *pPrimaryColumn,
                                     SField *pFields, __block_search_fn_t searchFn, int32_t *numOfRes) {
  int32_t forwardStep = 0;
  SQuery *pQuery = pRuntimeEnv->pQuery;

  int32_t step = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  validateQueryRangeAndData(pRuntimeEnv, pPrimaryColumn, pBlockInfo);

  if (QUERY_IS_ASC_QUERY(pQuery)) {
    if (pQuery->ekey < pBlockInfo->keyLast) {
      forwardStep = getForwardStepsInBlock(pBlockInfo->size, searchFn, pQuery, pPrimaryColumn);
      assert(forwardStep >= 0);

      if (forwardStep == 0) {
        // no qualified data in current block, do not update the lastKey value
        assert(pQuery->ekey < pPrimaryColumn[pQuery->pos]);
      } else {
        pQuery->lastKey = pQuery->ekey + step;//pPrimaryColumn[pQuery->pos + (forwardStep - 1)] + step;
      }

    } else {
      forwardStep = pBlockInfo->size - pQuery->pos;
      assert(forwardStep > 0);

      pQuery->lastKey = pBlockInfo->keyLast + step;
    }
  } else {  // desc
    if (pQuery->ekey > pBlockInfo->keyFirst) {
      forwardStep = getForwardStepsInBlock(pBlockInfo->size, searchFn, pQuery, pPrimaryColumn);
      assert(forwardStep >= 0);

      if (forwardStep == 0) {
        // no qualified data in current block, do not update the lastKey value
        assert(pQuery->ekey > pPrimaryColumn[pQuery->pos]);
      } else {
        pQuery->lastKey = pQuery->ekey + step;//pPrimaryColumn[pQuery->pos - (forwardStep - 1)] + step;
      }
    } else {
      forwardStep = pQuery->pos + 1;
      assert(forwardStep > 0);

      pQuery->lastKey = pBlockInfo->keyFirst + step;
    }
  }

  int32_t newForwardStep = reviseForwardSteps(pRuntimeEnv, forwardStep);
  assert(newForwardStep <= forwardStep && newForwardStep >= 0);

  // if buffer limitation is applied, there must be primary column(timestamp) loaded
  if (newForwardStep < forwardStep && newForwardStep > 0) {
    pQuery->lastKey = pPrimaryColumn[pQuery->pos + (newForwardStep - 1) * step] + step;
  }

  if (pQuery->numOfFilterCols > 0 || pRuntimeEnv->pTSBuf != NULL || isGroupbyNormalCol(pQuery->pGroupbyExpr) ||
      (pQuery->slidingTime != -1 && pQuery->nAggTimeInterval > 0)) {
    *numOfRes = rowwiseApplyAllFunctions(pRuntimeEnv, &newForwardStep, pPrimaryColumn, pFields, pBlockInfo);
  } else {
    *numOfRes = blockwiseApplyAllFunctions(pRuntimeEnv, newForwardStep, pPrimaryColumn, pFields, pBlockInfo);
  }

  assert(*numOfRes >= 0);

  // check if buffer is large enough for accommodating all qualified points
  if (*numOfRes > 0 && pQuery->checkBufferInLoop == 1) {
    pQuery->pointsOffset -= *numOfRes;
    if (pQuery->pointsOffset <= 0) {  // todo return correct numOfRes for ts_comp function
      pQuery->pointsOffset = 0;
      setQueryStatus(pQuery, QUERY_RESBUF_FULL);
    }
  }

  return newForwardStep;
}

int32_t vnodeGetVnodeHeaderFileIdx(int32_t *fid, SQueryRuntimeEnv *pRuntimeEnv, int32_t order) {
  if (pRuntimeEnv->vnodeFileInfo.numOfFiles == 0) {
    return -1;
  }

  SQueryFilesInfo *pVnodeFiles = &pRuntimeEnv->vnodeFileInfo;

  /* set the initial file for current query */
  if (order == TSQL_SO_ASC && *fid < pVnodeFiles->pFileInfo[0].fileID) {
    *fid = pVnodeFiles->pFileInfo[0].fileID;
    return 0;
  } else if (order == TSQL_SO_DESC && *fid > pVnodeFiles->pFileInfo[pVnodeFiles->numOfFiles - 1].fileID) {
    *fid = pVnodeFiles->pFileInfo[pVnodeFiles->numOfFiles - 1].fileID;
    return pVnodeFiles->numOfFiles - 1;
  }

  int32_t numOfFiles = pVnodeFiles->numOfFiles;

  if (order == TSQL_SO_DESC && *fid > pVnodeFiles->pFileInfo[numOfFiles - 1].fileID) {
    *fid = pVnodeFiles->pFileInfo[numOfFiles - 1].fileID;
    return numOfFiles - 1;
  }

  if (order == TSQL_SO_ASC) {
    int32_t i = 0;
    int32_t step = QUERY_ASC_FORWARD_STEP;

    while (i<numOfFiles && * fid> pVnodeFiles->pFileInfo[i].fileID) {
      i += step;
    }

    if (i < numOfFiles && *fid <= pVnodeFiles->pFileInfo[i].fileID) {
      *fid = pVnodeFiles->pFileInfo[i].fileID;
      return i;
    } else {
      return -1;
    }
  } else {
    int32_t i = numOfFiles - 1;
    int32_t step = QUERY_DESC_FORWARD_STEP;

    while (i >= 0 && *fid < pVnodeFiles->pFileInfo[i].fileID) {
      i += step;
    }

    if (i >= 0 && *fid >= pVnodeFiles->pFileInfo[i].fileID) {
      *fid = pVnodeFiles->pFileInfo[i].fileID;
      return i;
    } else {
      return -1;
    }
  }
}

int32_t getNextDataFileCompInfo(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, int32_t step) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  pQuery->fileId += step;

  int32_t fileIndex = 0;
  int32_t order = (step == QUERY_ASC_FORWARD_STEP) ? TSQL_SO_ASC : TSQL_SO_DESC;
  while (1) {
    fileIndex = vnodeGetVnodeHeaderFileIdx(&pQuery->fileId, pRuntimeEnv, order);

    // no files left, abort
    if (fileIndex < 0) {
      if (step == QUERY_ASC_FORWARD_STEP) {
        dTrace("QInfo:%p no more file to access, try data in cache", GET_QINFO_ADDR(pQuery));
      } else {
        dTrace("QInfo:%p no more file to access in desc order, query completed", GET_QINFO_ADDR(pQuery));
      }

      vnodeFreeFieldsEx(pRuntimeEnv);
      pQuery->fileId = -1;
      break;
    }

    // failed to mmap header file into memory will cause the retrieval of compblock info failed
    if (vnodeGetCompBlockInfo(pMeterObj, pRuntimeEnv, fileIndex) > 0) {
      break;
    }

    /*
     * 1. failed to read blk information from header file or open data file failed
     * 2. header file is empty
     *
     * try next one
     */
    pQuery->fileId += step;

    /* for backwards search, if the first file is not valid, abort */
    if (step < 0 && fileIndex == 0) {
      vnodeFreeFieldsEx(pRuntimeEnv);
      pQuery->fileId = -1;
      fileIndex = -1;
      break;
    }
  }

  return fileIndex;
}

void setExecParams(SQuery *pQuery, SQLFunctionCtx *pCtx, int64_t startQueryTimestamp, void *inputData,
                   char *primaryColumnData, int32_t size, int32_t functionId, SField *pField, bool hasNull,
                   int32_t blockStatus, void *param, int32_t scanFlag) {
  int32_t startOffset = (QUERY_IS_ASC_QUERY(pQuery)) ? pQuery->pos : pQuery->pos - (size - 1);

  pCtx->nStartQueryTimestamp = startQueryTimestamp;
  pCtx->scanFlag = scanFlag;

  pCtx->aInputElemBuf = inputData;
  pCtx->hasNull = hasNull;
  pCtx->blockStatus = blockStatus;

  if (pField != NULL) {
    pCtx->preAggVals.isSet = true;
    pCtx->preAggVals.minIndex = pField->minIndex;
    pCtx->preAggVals.maxIndex = pField->maxIndex;
    pCtx->preAggVals.sum = pField->sum;
    pCtx->preAggVals.max = pField->max;
    pCtx->preAggVals.min = pField->min;
    pCtx->preAggVals.numOfNull = pField->numOfNullPoints;
  } else {
    pCtx->preAggVals.isSet = false;
  }

  if ((aAggs[functionId].nStatus & TSDB_FUNCSTATE_SELECTIVITY) != 0 && (primaryColumnData != NULL)) {
    pCtx->ptsList = (int64_t *)(primaryColumnData + startOffset * TSDB_KEYSIZE);
  }

  if (functionId >= TSDB_FUNC_FIRST_DST && functionId <= TSDB_FUNC_LAST_DST) {
    // last_dist or first_dist function
    // store the first&last timestamp into the intermediate buffer [1], the true
    // value may be null but timestamp will never be null
    pCtx->ptsList = (int64_t *)(primaryColumnData + startOffset * TSDB_KEYSIZE);
  } else if (functionId == TSDB_FUNC_TOP      || functionId == TSDB_FUNC_BOTTOM ||
             functionId == TSDB_FUNC_TWA      || functionId == TSDB_FUNC_DIFF ||
             (functionId >= TSDB_FUNC_RATE    && functionId <= TSDB_FUNC_AVG_IRATE)) {
    /*
     * leastsquares function needs two columns of input, currently, the x value of linear equation is set to
     * timestamp column, and the y-value is the column specified in pQuery->pSelectExpr[i].colIdxInBuffer
     *
     * top/bottom function needs timestamp to indicate when the
     * top/bottom values emerge, so does diff function
     */
    if (functionId == TSDB_FUNC_TWA) {
      STwaInfo *pTWAInfo = GET_RES_INFO(pCtx)->interResultBuf;
      pTWAInfo->SKey = pQuery->skey;
      pTWAInfo->EKey = pQuery->ekey;
    }

    pCtx->ptsList = (int64_t *)(primaryColumnData + startOffset * TSDB_KEYSIZE);

  } else if (functionId == TSDB_FUNC_ARITHM) {
    pCtx->param[1].pz = param;
  }

  pCtx->startOffset = startOffset;
  pCtx->size = size;

#if defined(_DEBUG_VIEW)
  int64_t *tsList = (int64_t *)(primaryColumnData + startOffset * TSDB_KEYSIZE);
  int64_t  s = tsList[0];
  int64_t  e = tsList[size - 1];

//    if (IS_DATA_BLOCK_LOADED(blockStatus)) {
//        dTrace("QInfo:%p query ts:%lld-%lld, offset:%d, rows:%d, bstatus:%d,
//        functId:%d", GET_QINFO_ADDR(pQuery),
//               s, e, startOffset, size, blockStatus, functionId);
//    } else {
//        dTrace("QInfo:%p block not loaded, bstatus:%d",
//        GET_QINFO_ADDR(pQuery), blockStatus);
//    }
#endif
}

// set the output buffer for the selectivity + tag query
static void setCtxTagColumnInfo(SQuery *pQuery, SQueryRuntimeEnv *pRuntimeEnv) {
  if (isSelectivityWithTagsQuery(pQuery)) {
    int32_t         num = 0;
    SQLFunctionCtx *pCtx = NULL;
    int16_t         tagLen = 0;

    SQLFunctionCtx **pTagCtx = calloc(pQuery->numOfOutputCols, POINTER_BYTES);
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      SSqlFuncExprMsg *pSqlFuncMsg = &pQuery->pSelectExpr[i].pBase;
      if (pSqlFuncMsg->functionId == TSDB_FUNC_TAG_DUMMY || pSqlFuncMsg->functionId == TSDB_FUNC_TS_DUMMY) {
        tagLen += pRuntimeEnv->pCtx[i].outputBytes;
        pTagCtx[num++] = &pRuntimeEnv->pCtx[i];
      } else if ((aAggs[pSqlFuncMsg->functionId].nStatus & TSDB_FUNCSTATE_SELECTIVITY) != 0) {
        pCtx = &pRuntimeEnv->pCtx[i];
      } else if (pSqlFuncMsg->functionId == TSDB_FUNC_TS || pSqlFuncMsg->functionId == TSDB_FUNC_TAG) {
        // tag function may be the group by tag column
        // ts may be the required primary timestamp column
        continue;
      } else {
        // the column may be the normal column, group by normal_column, the functionId is TSDB_FUNC_PRJ
      }
    }

    pCtx->tagInfo.pTagCtxList = pTagCtx;
    pCtx->tagInfo.numOfTagCols = num;
    pCtx->tagInfo.tagsLen = tagLen;
  }
}

static int32_t setupQueryRuntimeEnv(SMeterObj *pMeterObj, SQuery *pQuery, SQueryRuntimeEnv *pRuntimeEnv,
                                    SSchema *pTagsSchema, int16_t order, bool isMetricQuery) {
  dTrace("QInfo:%p setup runtime env", GET_QINFO_ADDR(pQuery));

  pRuntimeEnv->pMeterObj = pMeterObj;
  pRuntimeEnv->pQuery = pQuery;

  pRuntimeEnv->resultInfo = calloc(pQuery->numOfOutputCols, sizeof(SResultInfo));
  pRuntimeEnv->pCtx = (SQLFunctionCtx *)calloc(pQuery->numOfOutputCols, sizeof(SQLFunctionCtx));

  if (pRuntimeEnv->resultInfo == NULL || pRuntimeEnv->pCtx == NULL) {
    goto _error_clean;
  }

  pRuntimeEnv->offset[0] = 0;
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    SSqlFuncExprMsg *pSqlFuncMsg = &pQuery->pSelectExpr[i].pBase;
    SColIndexEx *    pColIndexEx = &pSqlFuncMsg->colInfo;

    SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];

    if (TSDB_COL_IS_TAG(pSqlFuncMsg->colInfo.flag)) {  // process tag column info
      pCtx->inputType = pTagsSchema[pColIndexEx->colIdx].type;
      pCtx->inputBytes = pTagsSchema[pColIndexEx->colIdx].bytes;
    } else {
      pCtx->inputType = GET_COLUMN_TYPE(pQuery, i);
      pCtx->inputBytes = GET_COLUMN_BYTES(pQuery, i);
    }

    pCtx->ptsOutputBuf = NULL;

    pCtx->outputBytes = pQuery->pSelectExpr[i].resBytes;
    pCtx->outputType = pQuery->pSelectExpr[i].resType;

    pCtx->order = pQuery->order.order;
    pCtx->functionId = pSqlFuncMsg->functionId;

    pCtx->numOfParams = pSqlFuncMsg->numOfParams;
    for (int32_t j = 0; j < pCtx->numOfParams; ++j) {
      int16_t type = pSqlFuncMsg->arg[j].argType;
      int16_t bytes = pSqlFuncMsg->arg[j].argBytes;
      if (type == TSDB_DATA_TYPE_BINARY || type == TSDB_DATA_TYPE_NCHAR) {
        tVariantCreateFromBinary(&pCtx->param[j], pSqlFuncMsg->arg->argValue.pz, bytes, type);
      } else {
        tVariantCreateFromBinary(&pCtx->param[j], (char *)&pSqlFuncMsg->arg[j].argValue.i64, bytes, type);
      }
    }

    // set the order information for top/bottom query
    int32_t functionId = pCtx->functionId;

    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM || functionId == TSDB_FUNC_DIFF) {
      int32_t f = pQuery->pSelectExpr[0].pBase.functionId;
      assert(f == TSDB_FUNC_TS || f == TSDB_FUNC_TS_DUMMY);

      pCtx->param[2].i64Key = order;
      pCtx->param[2].nType = TSDB_DATA_TYPE_BIGINT;
      pCtx->param[3].i64Key = functionId;
      pCtx->param[3].nType = TSDB_DATA_TYPE_BIGINT;

      pCtx->param[1].i64Key = pQuery->order.orderColId;
    }

    if (i > 0) {
      pRuntimeEnv->offset[i] = pRuntimeEnv->offset[i - 1] + pRuntimeEnv->pCtx[i - 1].outputBytes;
    }

    // set the intermediate result output buffer
    SResultInfo *pResInfo = &pRuntimeEnv->resultInfo[i];
    setResultInfoBuf(pResInfo, pQuery->pSelectExpr[i].interResBytes, isMetricQuery);
  }

  // if it is group by normal column, do not set output buffer, the output buffer is pResult
  if (!isGroupbyNormalCol(pQuery->pGroupbyExpr) && !isMetricQuery) {
    resetCtxOutputBuf(pRuntimeEnv);
  }

  setCtxTagColumnInfo(pQuery, pRuntimeEnv);

  // for loading block data in memory
  assert(vnodeList[pMeterObj->vnode].cfg.rowsInFileBlock == pMeterObj->pointsPerFileBlock);
  return TSDB_CODE_SUCCESS;

_error_clean:
  tfree(pRuntimeEnv->resultInfo);
  tfree(pRuntimeEnv->pCtx);

  return TSDB_CODE_SERV_OUT_OF_MEMORY;
}

static void teardownQueryRuntimeEnv(SQueryRuntimeEnv *pRuntimeEnv) {
  if (pRuntimeEnv->pQuery == NULL) {
    return;
  }

  dTrace("QInfo:%p teardown runtime env", GET_QINFO_ADDR(pRuntimeEnv->pQuery));
  for (int32_t i = 0; i < pRuntimeEnv->pQuery->numOfCols; ++i) {
    tfree(pRuntimeEnv->colDataBuffer[i]);
  }

  tfree(pRuntimeEnv->secondaryUnzipBuffer);
  destroySlidingWindowInfo(&pRuntimeEnv->swindowResInfo);

  if (pRuntimeEnv->pCtx != NULL) {
    for (int32_t i = 0; i < pRuntimeEnv->pQuery->numOfOutputCols; ++i) {
      SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];

      for (int32_t j = 0; j < pCtx->numOfParams; ++j) {
        tVariantDestroy(&pCtx->param[j]);
      }

      tVariantDestroy(&pCtx->tag);
      tfree(pCtx->tagInfo.pTagCtxList);
      tfree(pRuntimeEnv->resultInfo[i].interResultBuf);
    }

    tfree(pRuntimeEnv->resultInfo);
    tfree(pRuntimeEnv->pCtx);
  }

  tfree(pRuntimeEnv->unzipBuffer);

  if (pRuntimeEnv->pQuery && (!PRIMARY_TSCOL_LOADED(pRuntimeEnv->pQuery))) {
    tfree(pRuntimeEnv->primaryColBuffer);
  }

  doCloseQueryFiles(&pRuntimeEnv->vnodeFileInfo);

  if (pRuntimeEnv->vnodeFileInfo.pFileInfo != NULL) {
    pRuntimeEnv->vnodeFileInfo.numOfFiles = 0;
    free(pRuntimeEnv->vnodeFileInfo.pFileInfo);
  }

  taosDestoryInterpoInfo(&pRuntimeEnv->interpoInfo);

  if (pRuntimeEnv->pInterpoBuf != NULL) {
    for (int32_t i = 0; i < pRuntimeEnv->pQuery->numOfOutputCols; ++i) {
      tfree(pRuntimeEnv->pInterpoBuf[i]);
    }

    tfree(pRuntimeEnv->pInterpoBuf);
  }

  pRuntimeEnv->pTSBuf = tsBufDestory(pRuntimeEnv->pTSBuf);
}

// get maximum time interval in each file
static int64_t getOldestKey(int32_t numOfFiles, int64_t fileId, SVnodeCfg *pCfg) {
  int64_t duration = pCfg->daysPerFile * tsMsPerDay[(uint8_t)pCfg->precision];
  return (fileId - numOfFiles + 1) * duration;
}

bool isQueryKilled(SQuery *pQuery) {
  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  /*
   * check if the queried meter is going to be deleted.
   * if it will be deleted soon, stop current query ASAP.
   */
  SMeterObj *pMeterObj = pQInfo->pObj;
  if (vnodeIsMeterState(pMeterObj, TSDB_METER_STATE_DROPPING)) {
    pQInfo->killed = 1;
    return true;
  }

  return (pQInfo->killed == 1);
}

bool isFixedOutputQuery(SQuery *pQuery) {
  if (pQuery->nAggTimeInterval != 0) {
    return false;
  }

  // Note:top/bottom query is fixed output query
  if (isTopBottomQuery(pQuery) || isGroupbyNormalCol(pQuery->pGroupbyExpr)) {
    return true;
  }

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    SSqlFuncExprMsg *pExprMsg = &pQuery->pSelectExpr[i].pBase;

    // ignore the ts_comp function
    if (i == 0 && pExprMsg->functionId == TSDB_FUNC_PRJ && pExprMsg->numOfParams == 1 &&
        pExprMsg->colInfo.colIdx == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
      continue;
    }

    if (pExprMsg->functionId == TSDB_FUNC_TS || pExprMsg->functionId == TSDB_FUNC_TS_DUMMY) {
      continue;
    }

    if (!IS_MULTIOUTPUT(aAggs[pExprMsg->functionId].nStatus)) {
      return true;
    }
  }

  return false;
}

bool isPointInterpoQuery(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionID = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionID == TSDB_FUNC_INTERP || functionID == TSDB_FUNC_LAST_ROW) {
      return true;
    }
  }

  return false;
}

// TODO REFACTOR:MERGE WITH CLIENT-SIDE FUNCTION
bool isSumAvgRateQuery(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TS) {
      continue;
    }

    if (functionId == TSDB_FUNC_SUM_RATE || functionId == TSDB_FUNC_SUM_IRATE || 
        functionId == TSDB_FUNC_AVG_RATE || functionId == TSDB_FUNC_AVG_IRATE) {
      return true;
    }
  }

  return false;
}

bool isTopBottomQuery(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TS) {
      continue;
    }

    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM) {
      return true;
    }
  }

  return false;
}

bool isFirstLastRowQuery(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionID = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionID == TSDB_FUNC_LAST_ROW) {
      return true;
    }
  }

  return false;
}

bool notHasQueryTimeRange(SQuery *pQuery) {
  return (pQuery->skey == 0 && pQuery->ekey == INT64_MAX && QUERY_IS_ASC_QUERY(pQuery)) ||
         (pQuery->skey == INT64_MAX && pQuery->ekey == 0 && (!QUERY_IS_ASC_QUERY(pQuery)));
}

bool isTSCompQuery(SQuery *pQuery) { return pQuery->pSelectExpr[0].pBase.functionId == TSDB_FUNC_TS_COMP; }

bool needSupplementaryScan(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TS || functionId == TSDB_FUNC_TS_DUMMY || functionId == TSDB_FUNC_TAG) {
      continue;
    }

    if (((functionId == TSDB_FUNC_LAST || functionId == TSDB_FUNC_LAST_DST) && QUERY_IS_ASC_QUERY(pQuery)) ||
        ((functionId == TSDB_FUNC_FIRST || functionId == TSDB_FUNC_FIRST_DST) && !QUERY_IS_ASC_QUERY(pQuery))) {
      return true;
    }
  }

  return false;
}
/////////////////////////////////////////////////////////////////////////////////////////////
static int32_t binarySearchInCacheBlk(SCacheInfo *pCacheInfo, SQuery *pQuery, int32_t keyLen, int32_t firstSlot,
                                      int32_t lastSlot) {
  int32_t midSlot = 0;

  while (1) {
    int32_t numOfBlocks = (lastSlot - firstSlot + 1 + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
    if (numOfBlocks == 0) {
      numOfBlocks = pCacheInfo->maxBlocks;
    }

    midSlot = (firstSlot + (numOfBlocks >> 1)) % pCacheInfo->maxBlocks;
    SCacheBlock *pBlock = pCacheInfo->cacheBlocks[midSlot];

    TSKEY keyFirst = *((TSKEY *)pBlock->offset[0]);
    TSKEY keyLast = *((TSKEY *)(pBlock->offset[0] + (pBlock->numOfPoints - 1) * keyLen));

    if (numOfBlocks == 1) {
      break;
    }

    if (pQuery->skey > keyLast) {
      if (numOfBlocks == 2) break;
      if (!QUERY_IS_ASC_QUERY(pQuery)) {
        int          nextSlot = (midSlot + 1 + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
        SCacheBlock *pNextBlock = pCacheInfo->cacheBlocks[nextSlot];
        TSKEY        nextKeyFirst = *((TSKEY *)(pNextBlock->offset[0]));
        if (pQuery->skey < nextKeyFirst) break;
      }
      firstSlot = (midSlot + 1) % pCacheInfo->maxBlocks;
    } else if (pQuery->skey < keyFirst) {
      if (QUERY_IS_ASC_QUERY(pQuery)) {
        int          prevSlot = (midSlot - 1 + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
        SCacheBlock *pPrevBlock = pCacheInfo->cacheBlocks[prevSlot];
        TSKEY        prevKeyLast = *((TSKEY *)(pPrevBlock->offset[0] + (pPrevBlock->numOfPoints - 1) * keyLen));
        if (pQuery->skey > prevKeyLast) {
          break;
        }
      }
      lastSlot = (midSlot - 1 + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
    } else {
      break;  // got the slot
    }
  }

  return midSlot;
}

static void getQueryRange(SQuery *pQuery, TSKEY *min, TSKEY *max) {
  *min = pQuery->lastKey < pQuery->ekey ? pQuery->lastKey : pQuery->ekey;
  *max = pQuery->lastKey >= pQuery->ekey ? pQuery->lastKey : pQuery->ekey;
}

static int32_t getFirstCacheSlot(int32_t numOfBlocks, int32_t lastSlot, SCacheInfo *pCacheInfo) {
  return (lastSlot - numOfBlocks + 1 + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
}

static bool cacheBoundaryCheck(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj) {
  /*
   * here we get the first slot from the meter cache, not from the cache snapshot from pQuery, since the
   * snapshot value in pQuery may have been expired now.
   */
  SQuery *pQuery = pRuntimeEnv->pQuery;

  SCacheInfo * pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
  SCacheBlock *pBlock = NULL;

  // earliest key in cache
  TSKEY keyFirst = 0;
  TSKEY keyLast = pMeterObj->lastKey;

  while (1) {
    // keep the value in local variable, since it may be changed by other thread any time
    int32_t numOfBlocks = pCacheInfo->numOfBlocks;
    int32_t currentSlot = pCacheInfo->currentSlot;

    // no data in cache, return false directly
    if (numOfBlocks == 0) {
      return false;
    }

    int32_t first = getFirstCacheSlot(numOfBlocks, currentSlot, pCacheInfo);

    /*
     * pBlock may be null value since this block is flushed to disk, and re-distributes to
     * other meter, so go on until we get the first not flushed cache block.
     */
    if ((pBlock = getCacheDataBlock(pMeterObj, pRuntimeEnv, first)) != NULL) {
      keyFirst = getTimestampInCacheBlock(pRuntimeEnv, pBlock, 0);
      break;
    } else {
      /*
       * there may be only one empty cache block existed caused by import.
       */
      if (numOfBlocks == 1) {
        return false;
      }
    }
  }

  TSKEY min, max;
  getQueryRange(pQuery, &min, &max);

  /*
   * The query time range is earlier than the first element or later than the last elements in cache.
   * If the query window overlaps with the time range of disk files, the flag needs to be reset.
   * Otherwise, this flag will cause error in following processing.
   */
  if (max < keyFirst || min > keyLast) {
    setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
    return false;
  }

  return true;
}

void getBasicCacheInfoSnapshot(SQuery *pQuery, SCacheInfo *pCacheInfo, int32_t vid) {
  // commitSlot here denotes the first uncommitted block in cache
  int32_t numOfBlocks = 0;
  int32_t lastSlot = 0;
  int32_t commitSlot = 0;
  int32_t commitPoint = 0;

  SCachePool *pPool = (SCachePool *)vnodeList[vid].pCachePool;
  pthread_mutex_lock(&pPool->vmutex);
  numOfBlocks = pCacheInfo->numOfBlocks;
  lastSlot = pCacheInfo->currentSlot;
  commitSlot = pCacheInfo->commitSlot;
  commitPoint = pCacheInfo->commitPoint;
  pthread_mutex_unlock(&pPool->vmutex);

  // make sure it is there, otherwise, return right away
  pQuery->currentSlot = lastSlot;
  pQuery->numOfBlocks = numOfBlocks;
  pQuery->firstSlot = getFirstCacheSlot(numOfBlocks, lastSlot, pCacheInfo);
  pQuery->commitSlot = commitSlot;
  pQuery->commitPoint = commitPoint;

  /*
   * Note: the block id is continuous increasing, never becomes smaller.
   *
   * blockId is the maximum block id in cache of current meter during query.
   * If any blocks' id are greater than this value, those blocks may be reallocated to other meters,
   * or assigned new data of this meter, on which the query is performed should be ignored.
   */
  if (pQuery->numOfBlocks > 0) {
    pQuery->blockId = pCacheInfo->cacheBlocks[pQuery->currentSlot]->blockId;
  }
}

int64_t getQueryStartPositionInCache(SQueryRuntimeEnv *pRuntimeEnv, int32_t *slot, int32_t *pos,
                                     bool ignoreQueryRange) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  pQuery->fileId = -1;
  vnodeFreeFieldsEx(pRuntimeEnv);

  // keep in-memory cache status in local variables in case that it may be changed by write operation
  getBasicCacheInfoSnapshot(pQuery, pMeterObj->pCache, pMeterObj->vnode);

  SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
  if (pCacheInfo == NULL || pCacheInfo->cacheBlocks == NULL || pQuery->numOfBlocks == 0) {
    setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
    return -1;
  }

  assert((pQuery->lastKey >= pQuery->skey && QUERY_IS_ASC_QUERY(pQuery)) ||
         (pQuery->lastKey <= pQuery->skey && !QUERY_IS_ASC_QUERY(pQuery)));

  if (!ignoreQueryRange && !cacheBoundaryCheck(pRuntimeEnv, pMeterObj)) {
    return -1;
  }

  /* find the appropriated slot that contains the requested points */
  TSKEY rawskey = pQuery->skey;

  /* here we actual start to query from pQuery->lastKey */
  pQuery->skey = pQuery->lastKey;

  (*slot) = binarySearchInCacheBlk(pCacheInfo, pQuery, TSDB_KEYSIZE, pQuery->firstSlot, pQuery->currentSlot);

  /* locate the first point of which time stamp is no less than pQuery->skey */
  __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];

  pQuery->slot = *slot;

  // cache block has been flushed to disk, no required data block in cache.
  SCacheBlock *pBlock = getCacheDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);
  if (pBlock == NULL) {
    pQuery->skey = rawskey;  // restore the skey
    return -1;
  }

  (*pos) = searchFn(pRuntimeEnv->primaryColBuffer->data, pBlock->numOfPoints, pQuery->skey, pQuery->order.order);

  // restore skey before return
  pQuery->skey = rawskey;

  // all data are less(greater) than the pQuery->lastKey in case of ascending(descending) query
  if (*pos == -1) {
    return -1;
  }

  int64_t nextKey = getTimestampInCacheBlock(pRuntimeEnv, pBlock, *pos);
  if ((nextKey < pQuery->lastKey && QUERY_IS_ASC_QUERY(pQuery)) ||
      (nextKey > pQuery->lastKey && !QUERY_IS_ASC_QUERY(pQuery))) {
    // all data are less than the pQuery->lastKey(pQuery->sKey) for asc query
    return -1;
  }

  SET_CACHE_BLOCK_FLAG(pRuntimeEnv->blockStatus);
  return nextKey;
}

/**
 * check if data in disk.
 */
bool hasDataInDisk(SQuery *pQuery, SMeterObj *pMeterObj) {
  SVnodeObj *pVnode = &vnodeList[pMeterObj->vnode];
  if (pVnode->numOfFiles <= 0) {
    pQuery->fileId = -1;
    return false;
  }

  int64_t latestKey = pMeterObj->lastKeyOnFile;
  int64_t oldestKey = getOldestKey(pVnode->numOfFiles, pVnode->fileId, &pVnode->cfg);

  TSKEY min, max;
  getQueryRange(pQuery, &min, &max);

  /* query range is out of current time interval of table */
  if ((min > latestKey) || (max < oldestKey)) {
    pQuery->fileId = -1;
    return false;
  }

  return true;
}

bool hasDataInCache(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj) {
  SQuery *    pQuery = pRuntimeEnv->pQuery;
  SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;

  /* no data in cache, return */
  if ((pCacheInfo == NULL) || (pCacheInfo->cacheBlocks == NULL)) {
    return false;
  }

  /* numOfBlocks value has been overwrite, release pFields data if exists */
  vnodeFreeFieldsEx(pRuntimeEnv);
  getBasicCacheInfoSnapshot(pQuery, pCacheInfo, pMeterObj->vnode);
  if (pQuery->numOfBlocks <= 0) {
    return false;
  }

  return cacheBoundaryCheck(pRuntimeEnv, pMeterObj);
}

/**
 *  Get cache snapshot will destroy the comp block info in SQuery, in order to speedup the query
 *  process, we always check cache first.
 */
void vnodeCheckIfDataExists(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj, bool *dataInDisk, bool *dataInCache) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  *dataInCache = hasDataInCache(pRuntimeEnv, pMeterObj);
  *dataInDisk = hasDataInDisk(pQuery, pMeterObj);

  setQueryStatus(pQuery, QUERY_NOT_COMPLETED);
}

static void doGetAlignedIntervalQueryRangeImpl(SQuery *pQuery, int64_t pKey, int64_t keyFirst, int64_t keyLast,
                                               int64_t *actualSkey, int64_t *actualEkey, int64_t *skey, int64_t *ekey) {
  assert(pKey >= keyFirst && pKey <= keyLast);
  *skey = taosGetIntervalStartTimestamp(pKey, pQuery->nAggTimeInterval, pQuery->intervalTimeUnit, pQuery->precision);

  if (keyFirst > (INT64_MAX - pQuery->nAggTimeInterval)) {
    /*
     * if the actualSkey > INT64_MAX - pQuery->nAggTimeInterval, the query duration between
     * actualSkey and actualEkey must be less than one interval.Therefore, no need to adjust the query ranges.
     */
    assert(keyLast - keyFirst < pQuery->nAggTimeInterval);

    *actualSkey = keyFirst;
    *actualEkey = keyLast;

    *ekey = INT64_MAX;
    return;
  }

  *ekey = *skey + pQuery->nAggTimeInterval - 1;

  if (*skey < keyFirst) {
    *actualSkey = keyFirst;
  } else {
    *actualSkey = *skey;
  }

  if (*ekey < keyLast) {
    *actualEkey = *ekey;
  } else {
    *actualEkey = keyLast;
  }
}

static void getAlignedIntervalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, TSKEY key, TSKEY skey, TSKEY ekey) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  if (pQuery->nAggTimeInterval == 0 || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    return;
  }

  TSKEY skey2 = MIN(skey, ekey);
  TSKEY ekey2 = MAX(skey, ekey);

  // the actual first query range in skey1 and ekey1
  TSKEY skey1, ekey1;

  TSKEY windowSKey = 0, windowEKey = 0;
  doGetAlignedIntervalQueryRangeImpl(pQuery, key, skey2, ekey2, &skey1, &ekey1, &windowSKey, &windowEKey);

  if (QUERY_IS_ASC_QUERY(pQuery)) {
    pQuery->skey = skey1;
    pQuery->ekey = ekey1;

    pRuntimeEnv->intervalWindow = (STimeWindow) {.skey = windowSKey, .ekey = windowEKey};

    assert(pQuery->skey <= pQuery->ekey &&
           pRuntimeEnv->intervalWindow.skey + (pQuery->nAggTimeInterval - 1) == pRuntimeEnv->intervalWindow.ekey);
  } else {
    pQuery->skey = ekey1;
    pQuery->ekey = skey1;
  
    pRuntimeEnv->intervalWindow = (STimeWindow) {.skey = windowEKey, .ekey = windowSKey};
  
    assert(pQuery->skey >= pQuery->ekey &&
           pRuntimeEnv->intervalWindow.skey - (pQuery->nAggTimeInterval - 1) == pRuntimeEnv->intervalWindow.ekey);
  }

  pQuery->lastKey = pQuery->skey;
}

static void getOneRowFromDataBlock(SQueryRuntimeEnv *pRuntimeEnv, char **dst, int32_t pos) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
    int32_t bytes = pQuery->colList[i].data.bytes;
    memcpy(dst[i], pRuntimeEnv->colDataBuffer[i]->data + pos * bytes, bytes);
  }
}

static bool getNeighborPoints(SMeterQuerySupportObj *pSupporter, SMeterObj *pMeterObj,
                              SPointInterpoSupporter *pPointInterpSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  if (!isPointInterpoQuery(pQuery)) {
    return false;
  }

  /*
   * for interpolate point query, points that are directly before/after the specified point are required
   */
  if (isFirstLastRowQuery(pQuery)) {
    assert(!QUERY_IS_ASC_QUERY(pQuery));
  } else {
    assert(QUERY_IS_ASC_QUERY(pQuery));
  }
  assert(pPointInterpSupporter != NULL && pQuery->skey == pQuery->ekey);

  SCacheBlock *pBlock = NULL;

  qTrace("QInfo:%p get next data point, fileId:%d, slot:%d, pos:%d", GET_QINFO_ADDR(pQuery), pQuery->fileId,
         pQuery->slot, pQuery->pos);

  // save the point that is directly after or equals to the specified point
  getOneRowFromDataBlock(pRuntimeEnv, pPointInterpSupporter->pNextPoint, pQuery->pos);

  /*
   * 1. for last_row query, return immediately.
   * 2. the specified timestamp equals to the required key, interpolation according to neighbor points is not necessary
   *    for interp query.
   */
  TSKEY actualKey = *(TSKEY *)pPointInterpSupporter->pNextPoint[0];
  if (isFirstLastRowQuery(pQuery) || actualKey == pQuery->skey) {
    setQueryStatus(pQuery, QUERY_NOT_COMPLETED);

    /*
     * the retrieved ts may not equals to pMeterObj->lastKey due to cache re-allocation
     * set the pQuery->ekey/pQuery->skey/pQuery->lastKey to be the new value.
     */
    if (pQuery->ekey != actualKey) {
      pQuery->skey = actualKey;
      pQuery->ekey = actualKey;
      pQuery->lastKey = actualKey;
      pSupporter->rawSKey = actualKey;
      pSupporter->rawEKey = actualKey;
    }
    return true;
  }

  /* the qualified point is not the first point in data block */
  if (pQuery->pos > 0) {
    int32_t prevPos = pQuery->pos - 1;

    /* save the point that is directly after the specified point */
    getOneRowFromDataBlock(pRuntimeEnv, pPointInterpSupporter->pPrevPoint, prevPos);
  } else {
    __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];

    savePointPosition(&pRuntimeEnv->startPos, pQuery->fileId, pQuery->slot, pQuery->pos);

    // backwards movement would not set the pQuery->pos correct. We need to set it manually later.
    moveToNextBlock(pRuntimeEnv, QUERY_DESC_FORWARD_STEP, searchFn, true);

    /*
     * no previous data exists.
     * reset the status and load the data block that contains the qualified point
     */
    if (Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK)) {
      dTrace("QInfo:%p no previous data block, start fileId:%d, slot:%d, pos:%d, qrange:%" PRId64 "-%" PRId64
             ", out of range",
             GET_QINFO_ADDR(pQuery), pRuntimeEnv->startPos.fileId, pRuntimeEnv->startPos.slot,
             pRuntimeEnv->startPos.pos, pQuery->skey, pQuery->ekey);

      // no result, return immediately
      setQueryStatus(pQuery, QUERY_COMPLETED);
      return false;
    } else {  // prev has been located
      if (pQuery->fileId >= 0) {
        pQuery->pos = pQuery->pBlock[pQuery->slot].numOfPoints - 1;
        getOneRowFromDataBlock(pRuntimeEnv, pPointInterpSupporter->pPrevPoint, pQuery->pos);

        qTrace("QInfo:%p get prev data point, fileId:%d, slot:%d, pos:%d, pQuery->pos:%d", GET_QINFO_ADDR(pQuery),
               pQuery->fileId, pQuery->slot, pQuery->pos, pQuery->pos);
      } else {
        // moveToNextBlock make sure there is a available cache block, if exists
        assert(vnodeIsDatablockLoaded(pRuntimeEnv, pMeterObj, -1, true) == DISK_BLOCK_NO_NEED_TO_LOAD);
        pBlock = &pRuntimeEnv->cacheBlock;

        pQuery->pos = pBlock->numOfPoints - 1;
        getOneRowFromDataBlock(pRuntimeEnv, pPointInterpSupporter->pPrevPoint, pQuery->pos);

        qTrace("QInfo:%p get prev data point, fileId:%d, slot:%d, pos:%d, pQuery->pos:%d", GET_QINFO_ADDR(pQuery),
               pQuery->fileId, pQuery->slot, pBlock->numOfPoints - 1, pQuery->pos);
      }
    }
  }

  pQuery->skey = *(TSKEY *)pPointInterpSupporter->pPrevPoint[0];
  pQuery->ekey = *(TSKEY *)pPointInterpSupporter->pNextPoint[0];
  pQuery->lastKey = pQuery->skey;

  return true;
}

static bool doGetQueryPos(TSKEY key, SMeterQuerySupportObj *pSupporter, SPointInterpoSupporter *pPointInterpSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;
  SMeterObj *       pMeterObj = pRuntimeEnv->pMeterObj;

  /* key in query range. If not, no qualified in disk file */
  if (key != -1 && key <= pQuery->ekey) {
    if (isPointInterpoQuery(pQuery)) { /* no qualified data in this query range */
      return getNeighborPoints(pSupporter, pMeterObj, pPointInterpSupporter);
    } else {
      getAlignedIntervalQueryRange(pRuntimeEnv, key, pQuery->skey, pQuery->ekey);
      return true;
    }
  } else {  // key > pQuery->ekey, abort for normal query, continue for interp query
    if (isPointInterpoQuery(pQuery)) {
      return getNeighborPoints(pSupporter, pMeterObj, pPointInterpSupporter);
    } else {
      return false;
    }
  }
}

static bool doSetDataInfo(SMeterQuerySupportObj *pSupporter, SPointInterpoSupporter *pPointInterpSupporter,
                          SMeterObj *pMeterObj, TSKEY nextKey) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  if (isFirstLastRowQuery(pQuery)) {
    /*
     * if the pQuery->skey != pQuery->ekey for last_row query,
     * the query range is existed, so set them both the value of nextKey
     */
    if (pQuery->skey != pQuery->ekey) {
      assert(pQuery->skey >= pQuery->ekey && !QUERY_IS_ASC_QUERY(pQuery) && nextKey >= pQuery->ekey &&
             nextKey <= pQuery->skey);

      pQuery->skey = nextKey;
      pQuery->ekey = nextKey;
    }

    return getNeighborPoints(pSupporter, pMeterObj, pPointInterpSupporter);
  } else {
    getAlignedIntervalQueryRange(pRuntimeEnv, nextKey, pQuery->skey, pQuery->ekey);
    return true;
  }
}

// TODO refactor code, the best way to implement the last_row is utilizing the iterator
bool normalizeUnBoundLastRowQuery(SMeterQuerySupportObj *pSupporter, SPointInterpoSupporter *pPointInterpSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;

  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  assert(!QUERY_IS_ASC_QUERY(pQuery) && notHasQueryTimeRange(pQuery));
  __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];

  TSKEY lastKey = -1;

  pQuery->fileId = -1;
  vnodeFreeFieldsEx(pRuntimeEnv);

  // keep in-memory cache status in local variables in case that it may be changed by write operation
  getBasicCacheInfoSnapshot(pQuery, pMeterObj->pCache, pMeterObj->vnode);

  SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
  if (pCacheInfo != NULL && pCacheInfo->cacheBlocks != NULL && pQuery->numOfBlocks > 0) {
    pQuery->fileId = -1;
    TSKEY key = pMeterObj->lastKey;

    pQuery->skey = key;
    pQuery->ekey = key;
    pQuery->lastKey = pQuery->skey;

    /*
     * cache block may have been flushed to disk, and no data in cache anymore.
     * So, copy cache block to local buffer is required.
     */
    lastKey = getQueryStartPositionInCache(pRuntimeEnv, &pQuery->slot, &pQuery->pos, false);
    if (lastKey < 0) {  // data has been flushed to disk, try again search in file
      lastKey = getQueryPositionForCacheInvalid(pRuntimeEnv, searchFn);

      if (Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK | QUERY_COMPLETED)) {
        return false;
      }
    }
  } else {  // no data in cache, try file
    TSKEY key = pMeterObj->lastKeyOnFile;

    pQuery->skey = key;
    pQuery->ekey = key;
    pQuery->lastKey = pQuery->skey;

    bool ret = getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_LESS_EQUAL, searchFn);
    if (!ret) {  // no data in file, return false;
      return false;
    }

    lastKey = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);
  }

  assert(lastKey <= pQuery->skey);

  pQuery->skey = lastKey;
  pQuery->ekey = lastKey;
  pQuery->lastKey = pQuery->skey;

  return getNeighborPoints(pSupporter, pMeterObj, pPointInterpSupporter);
}

/**
 * determine the first query range, according to raw query range [skey, ekey] and group-by interval.
 * the time interval for aggregating is not enforced to check its validation, the minimum interval is not less than
 * 10ms, which is guaranteed by parser at client-side
 */
bool normalizedFirstQueryRange(bool dataInDisk, bool dataInCache, SMeterQuerySupportObj *pSupporter,
                               SPointInterpoSupporter *pPointInterpSupporter, int64_t *key) {
  SQueryRuntimeEnv *  pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *            pQuery = pRuntimeEnv->pQuery;
  SMeterObj *         pMeterObj = pRuntimeEnv->pMeterObj;
  __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];

  if (QUERY_IS_ASC_QUERY(pQuery)) {
    // todo: the action return as the getQueryStartPositionInCache function
    if (dataInDisk && getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_GREATER_EQUAL, searchFn)) {
      TSKEY nextKey = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);
      assert(nextKey >= pQuery->skey);

      if (key != NULL) {
        *key = nextKey;
      }

      return doGetQueryPos(nextKey, pSupporter, pPointInterpSupporter);
    }

    // set no data in file
    pQuery->fileId = -1;
    SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;

    /* if it is a interpolation query, the any points in cache that is greater than the query range is required */
    if (pCacheInfo == NULL || pCacheInfo->cacheBlocks == NULL || pCacheInfo->numOfBlocks == 0 || !dataInCache) {
      return false;
    }

    TSKEY nextKey = getQueryStartPositionInCache(pRuntimeEnv, &pQuery->slot, &pQuery->pos, false);

    if (key != NULL) {
      *key = nextKey;
    }

    return doGetQueryPos(nextKey, pSupporter, pPointInterpSupporter);

  } else {              // descending order
    if (dataInCache) {  // todo handle error
      TSKEY nextKey = getQueryStartPositionInCache(pRuntimeEnv, &pQuery->slot, &pQuery->pos, false);
      assert(nextKey == -1 || nextKey <= pQuery->skey);

      if (key != NULL) {
        *key = nextKey;
      }

      if (nextKey != -1) {  // find qualified data in cache
        if (nextKey >= pQuery->ekey) {
          return doSetDataInfo(pSupporter, pPointInterpSupporter, pMeterObj, nextKey);
        } else {
          /*
           * nextKey < pQuery->ekey && nextKey < pQuery->lastKey, query range is
           * larger than all data, abort
           *
           * NOTE: Interp query does not reach here, since for all interp query,
           * the query order is ascending order.
           */
          return false;
        }
      } else {  // all data in cache are greater than pQuery->skey, try file
      }
    }

    if (dataInDisk && getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_LESS_EQUAL, searchFn)) {
      TSKEY nextKey = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);
      assert(nextKey <= pQuery->skey);

      if (key != NULL) {
        *key = nextKey;
      }

      // key in query range. If not, no qualified in disk file
      if (nextKey >= pQuery->ekey) {
        return doSetDataInfo(pSupporter, pPointInterpSupporter, pMeterObj, nextKey);
      } else {  // In case of all queries, the value of false will be returned if key < pQuery->ekey
        return false;
      }
    }
  }

  return false;
}

int64_t loadRequiredBlockIntoMem(SQueryRuntimeEnv *pRuntimeEnv, SPositionInfo *position) {
  TSKEY nextTimestamp = -1;

  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  pQuery->fileId = position->fileId;
  pQuery->slot = position->slot;
  pQuery->pos = position->pos;

  if (position->fileId == -1) {
    SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
    if (pCacheInfo == NULL || pCacheInfo->numOfBlocks == 0 || pCacheInfo->cacheBlocks == NULL) {
      setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
      return -1;
    }

    SCacheBlock *pBlock = getCacheDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);
    if (pBlock != NULL) {
      nextTimestamp = getTimestampInCacheBlock(pRuntimeEnv, pBlock, position->pos);
    } else {
      // todo fix it
    }

    SET_CACHE_BLOCK_FLAG(pRuntimeEnv->blockStatus);
  } else {
    // todo handle the file broken situation
    /*
     * load the file metadata into buffer first, then the specific data block.
     * currently opened file is not the start file, reset to the start file
     */
    int32_t fileIdx = vnodeGetVnodeHeaderFileIdx(&pQuery->fileId, pRuntimeEnv, pQuery->order.order);
    if (fileIdx < 0) {  // ignore the files on disk
      dError("QInfo:%p failed to get data file:%d", GET_QINFO_ADDR(pQuery), pQuery->fileId);
      position->fileId = -1;
      return -1;
    }

    /*
     * NOTE:
     * The compblock information may not be loaded yet, here loaded it firstly.
     * If the compBlock info is loaded, it wont be loaded again.
     *
     * If failed to load comp block into memory due some how reasons, e.g., empty header file/not enough memory
     */
    if (vnodeGetCompBlockInfo(pMeterObj, pRuntimeEnv, fileIdx) <= 0) {
      position->fileId = -1;
      return -1;
    }

    nextTimestamp = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);
  }

  return nextTimestamp;
}

static void setScanLimitationByResultBuffer(SQuery *pQuery) {
  if (isTopBottomQuery(pQuery)) {
    pQuery->checkBufferInLoop = 0;
  } else if (isGroupbyNormalCol(pQuery->pGroupbyExpr)) {
    pQuery->checkBufferInLoop = 0;
  } else {
    bool hasMultioutput = false;
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      SSqlFuncExprMsg *pExprMsg = &pQuery->pSelectExpr[i].pBase;
      if (pExprMsg->functionId == TSDB_FUNC_TS || pExprMsg->functionId == TSDB_FUNC_TS_DUMMY) {
        continue;
      }

      hasMultioutput = IS_MULTIOUTPUT(aAggs[pExprMsg->functionId].nStatus);
      if (!hasMultioutput) {
        break;
      }
    }

    pQuery->checkBufferInLoop = hasMultioutput ? 1 : 0;
  }

  pQuery->pointsOffset = pQuery->pointsToRead;
}

/*
 * todo add more parameters to check soon..
 */
bool vnodeParametersSafetyCheck(SQuery *pQuery) {
  // load data column information is incorrect
  for (int32_t i = 0; i < pQuery->numOfCols - 1; ++i) {
    if (pQuery->colList[i].data.colId == pQuery->colList[i + 1].data.colId) {
      dError("QInfo:%p invalid data load column for query", GET_QINFO_ADDR(pQuery));
      return false;
    }
  }
  return true;
}

static int file_order_comparator(const void *p1, const void *p2) {
  SHeaderFileInfo *pInfo1 = (SHeaderFileInfo *)p1;
  SHeaderFileInfo *pInfo2 = (SHeaderFileInfo *)p2;

  if (pInfo1->fileID == pInfo2->fileID) {
    return 0;
  }

  return (pInfo1->fileID > pInfo2->fileID) ? 1 : -1;
}

/**
 * open a data files and header file for metric meta query
 *
 * @param pVnodeFiles
 * @param fid
 * @param index
 */
static FORCE_INLINE void vnodeStoreFileId(SQueryFilesInfo *pVnodeFiles, int32_t fid, int32_t index) {
  pVnodeFiles->pFileInfo[index].fileID = fid;
}

static void vnodeRecordAllFiles(SQInfo *pQInfo, int32_t vnodeId) {
  char suffix[] = ".head";

  struct dirent *pEntry = NULL;
  size_t         alloc = 4;  // default allocated size

  SQueryFilesInfo *pVnodeFilesInfo = &(pQInfo->pMeterQuerySupporter->runtimeEnv.vnodeFileInfo);
  pVnodeFilesInfo->vnodeId = vnodeId;

  sprintf(pVnodeFilesInfo->dbFilePathPrefix, "%s/vnode%d/db/", tsDirectory, vnodeId);
  DIR *pDir = opendir(pVnodeFilesInfo->dbFilePathPrefix);
  if (pDir == NULL) {
    dError("QInfo:%p failed to open directory:%s, %s", pQInfo, pVnodeFilesInfo->dbFilePathPrefix, strerror(errno));
    return;
  }

  pVnodeFilesInfo->pFileInfo = calloc(1, sizeof(SHeaderFileInfo) * alloc);
  SVnodeObj *pVnode = &vnodeList[vnodeId];

  while ((pEntry = readdir(pDir)) != NULL) {
    if ((pEntry->d_name[0] == '.' && pEntry->d_name[1] == '\0') || (strcmp(pEntry->d_name, "..") == 0)) {
      continue;
    }

    if (pEntry->d_type & DT_DIR) {
      continue;
    }

    size_t len = strlen(pEntry->d_name);
    if (strcasecmp(&pEntry->d_name[len - 5], suffix) != 0) {
      continue;
    }

    int32_t vid = 0;
    int32_t fid = 0;
    sscanf(pEntry->d_name, "v%df%d", &vid, &fid);
    if (vid != vnodeId) { /* ignore error files */
      dError("QInfo:%p error data file:%s in vid:%d, ignore", pQInfo, pEntry->d_name, vnodeId);
      continue;
    }

    int32_t firstFid = pVnode->fileId - pVnode->numOfFiles + 1;
    if (fid > pVnode->fileId || fid < firstFid) {
      dError("QInfo:%p error data file:%s in vid:%d, fid:%d, fid range:%d-%d", pQInfo, pEntry->d_name, vnodeId, fid,
             firstFid, pVnode->fileId);
      continue;
    }

    assert(fid >= 0 && vid >= 0);

    if (++pVnodeFilesInfo->numOfFiles > alloc) {
      alloc = alloc << 1U;
      pVnodeFilesInfo->pFileInfo = realloc(pVnodeFilesInfo->pFileInfo, alloc * sizeof(SHeaderFileInfo));
      memset(&pVnodeFilesInfo->pFileInfo[alloc >> 1U], 0, (alloc >> 1U) * sizeof(SHeaderFileInfo));
    }

    int32_t index = pVnodeFilesInfo->numOfFiles - 1;
    vnodeStoreFileId(pVnodeFilesInfo, fid, index);
  }

  closedir(pDir);

  dTrace("QInfo:%p find %d data files in %s to be checked", pQInfo, pVnodeFilesInfo->numOfFiles,
         pVnodeFilesInfo->dbFilePathPrefix);

  /* order the files information according their names */
  qsort(pVnodeFilesInfo->pFileInfo, (size_t)pVnodeFilesInfo->numOfFiles, sizeof(SHeaderFileInfo),
        file_order_comparator);
}

static void updateOffsetVal(SQueryRuntimeEnv *pRuntimeEnv, SBlockInfo *pBlockInfo, void *pBlock) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  /*
   *  The actually qualified points that can be skipped needs to be calculated if query is
   *  done in current data block
   */
  if ((pQuery->ekey <= pBlockInfo->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
      (pQuery->ekey >= pBlockInfo->keyFirst && !QUERY_IS_ASC_QUERY(pQuery))) {
    // force load timestamp data blocks
    if (IS_DISK_DATA_BLOCK(pQuery)) {
      getTimestampInDiskBlock(pRuntimeEnv, 0);
    }

    // update the pQuery->limit.offset value, and pQuery->pos value
    TSKEY *keys = (TSKEY *)pRuntimeEnv->primaryColBuffer->data;

    int32_t i = 0;
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      for (i = pQuery->pos; i < pBlockInfo->size && pQuery->limit.offset > 0; ++i) {
        if (keys[i] <= pQuery->ekey) {
          pQuery->limit.offset -= 1;
        } else {
          break;
        }
      }

    } else {
      for (i = pQuery->pos; i >= 0 && pQuery->limit.offset > 0; --i) {
        if (keys[i] >= pQuery->ekey) {
          pQuery->limit.offset -= 1;
        } else {
          break;
        }
      }
    }

    if (((i == pBlockInfo->size || keys[i] > pQuery->ekey) && QUERY_IS_ASC_QUERY(pQuery)) ||
        ((i < 0 || keys[i] < pQuery->ekey) && !QUERY_IS_ASC_QUERY(pQuery))) {
      setQueryStatus(pQuery, QUERY_COMPLETED);
      pQuery->pos = -1;
    } else {
      pQuery->pos = i;
    }
  } else {
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      pQuery->pos += pQuery->limit.offset;
    } else {
      pQuery->pos -= pQuery->limit.offset;
    }

    assert(pQuery->pos >= 0 && pQuery->pos <= pBlockInfo->size - 1);

    if (IS_DISK_DATA_BLOCK(pQuery)) {
      pQuery->skey = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);
    } else {
      pQuery->skey = getTimestampInCacheBlock(pRuntimeEnv, pBlock, pQuery->pos);
    }

    // update the offset value
    pQuery->lastKey = pQuery->skey;
    pQuery->limit.offset = 0;
  }
}

// todo ignore the avg/sum/min/max/count/stddev/top/bottom functions, of which
// the scan order is not matter
static bool onlyOneQueryType(SQuery *pQuery, int32_t functId, int32_t functIdDst) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;

    if (functionId == TSDB_FUNC_TS || functionId == TSDB_FUNC_TS_DUMMY || functionId == TSDB_FUNC_TAG ||
        functionId == TSDB_FUNC_TAG_DUMMY) {
      continue;
    }

    if (functionId != functId && functionId != functIdDst) {
      return false;
    }
  }

  return true;
}

static bool onlyFirstQuery(SQuery *pQuery) { return onlyOneQueryType(pQuery, TSDB_FUNC_FIRST, TSDB_FUNC_FIRST_DST); }

static bool onlyLastQuery(SQuery *pQuery) { return onlyOneQueryType(pQuery, TSDB_FUNC_LAST, TSDB_FUNC_LAST_DST); }

static void changeExecuteScanOrder(SQuery *pQuery, bool metricQuery) {
  // in case of point-interpolation query, use asc order scan
  char msg[] =
      "QInfo:%p scan order changed for %s query, old:%d, new:%d, qrange exchanged, old qrange:%" PRId64 "-%" PRId64
      ", "
      "new qrange:%" PRId64 "-%" PRId64;

  // descending order query for last_row query
  if (isFirstLastRowQuery(pQuery)) {
    dTrace("QInfo:%p scan order changed for last_row query, old:%d, new:%d", GET_QINFO_ADDR(pQuery),
           pQuery->order.order, TSQL_SO_DESC);

    pQuery->order.order = TSQL_SO_DESC;

    int64_t skey = MIN(pQuery->skey, pQuery->ekey);
    int64_t ekey = MAX(pQuery->skey, pQuery->ekey);

    pQuery->skey = ekey;
    pQuery->ekey = skey;

    return;
  }

  if (isPointInterpoQuery(pQuery) && pQuery->nAggTimeInterval == 0) {
    if (!QUERY_IS_ASC_QUERY(pQuery)) {
      dTrace(msg, GET_QINFO_ADDR(pQuery), "interp", pQuery->order.order, TSQL_SO_ASC, pQuery->skey, pQuery->ekey,
             pQuery->ekey, pQuery->skey);
      SWAP(pQuery->skey, pQuery->ekey, TSKEY);
    }

    pQuery->order.order = TSQL_SO_ASC;
    return;
  }

  if (pQuery->nAggTimeInterval == 0) {
    if (onlyFirstQuery(pQuery)) {
      if (!QUERY_IS_ASC_QUERY(pQuery)) {
        dTrace(msg, GET_QINFO_ADDR(pQuery), "only-first", pQuery->order.order, TSQL_SO_ASC, pQuery->skey, pQuery->ekey,
               pQuery->ekey, pQuery->skey);

        SWAP(pQuery->skey, pQuery->ekey, TSKEY);
      }

      pQuery->order.order = TSQL_SO_ASC;
    } else if (onlyLastQuery(pQuery)) {
      if (QUERY_IS_ASC_QUERY(pQuery)) {
        dTrace(msg, GET_QINFO_ADDR(pQuery), "only-last", pQuery->order.order, TSQL_SO_DESC, pQuery->skey, pQuery->ekey,
               pQuery->ekey, pQuery->skey);

        SWAP(pQuery->skey, pQuery->ekey, TSKEY);
      }

      pQuery->order.order = TSQL_SO_DESC;
    }

  } else {  // interval query
    if (metricQuery) {
      if (onlyFirstQuery(pQuery)) {
        if (!QUERY_IS_ASC_QUERY(pQuery)) {
          dTrace(msg, GET_QINFO_ADDR(pQuery), "only-first stable", pQuery->order.order, TSQL_SO_ASC, pQuery->skey,
                 pQuery->ekey, pQuery->ekey, pQuery->skey);

          SWAP(pQuery->skey, pQuery->ekey, TSKEY);
        }

        pQuery->order.order = TSQL_SO_ASC;
      } else if (onlyLastQuery(pQuery)) {
        if (QUERY_IS_ASC_QUERY(pQuery)) {
          dTrace(msg, GET_QINFO_ADDR(pQuery), "only-last stable", pQuery->order.order, TSQL_SO_DESC, pQuery->skey,
                 pQuery->ekey, pQuery->ekey, pQuery->skey);

          SWAP(pQuery->skey, pQuery->ekey, TSKEY);
        }

        pQuery->order.order = TSQL_SO_DESC;
      }
    }
  }
}

static int32_t doSkipDataBlock(SQueryRuntimeEnv *pRuntimeEnv) {
  SMeterObj *         pMeterObj = pRuntimeEnv->pMeterObj;
  SQuery *            pQuery = pRuntimeEnv->pQuery;
  int32_t             step = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);
  __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];

  while (1) {
    moveToNextBlock(pRuntimeEnv, step, searchFn, false);
    if (Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK)) {
      break;
    }

    void *pBlock = getGenericDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);
    assert(pBlock != NULL);

    int32_t    blockType = IS_DISK_DATA_BLOCK(pQuery) ? BLK_FILE_BLOCK : BLK_CACHE_BLOCK;
    SBlockInfo blockInfo = getBlockBasicInfo(pRuntimeEnv, pBlock, blockType);

    int32_t maxReads = (QUERY_IS_ASC_QUERY(pQuery)) ? blockInfo.size - pQuery->pos : pQuery->pos + 1;
    assert(maxReads >= 0);

    if (pQuery->limit.offset < maxReads || (pQuery->ekey <= blockInfo.keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
        (pQuery->ekey >= blockInfo.keyFirst && !QUERY_IS_ASC_QUERY(pQuery))) {  // start position in current block
      updateOffsetVal(pRuntimeEnv, &blockInfo, pBlock);
      break;
    } else {
      pQuery->limit.offset -= maxReads;
      pQuery->lastKey = (QUERY_IS_ASC_QUERY(pQuery)) ? blockInfo.keyLast : blockInfo.keyFirst;
      pQuery->lastKey += step;

      qTrace("QInfo:%p skip rows:%d, offset:%" PRId64 "", GET_QINFO_ADDR(pQuery), maxReads, pQuery->limit.offset);
    }
  }

  return 0;
}

void forwardQueryStartPosition(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  if (pQuery->limit.offset <= 0) {
    return;
  }

  void *pBlock = getGenericDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);

  int32_t    blockType = (IS_DISK_DATA_BLOCK(pQuery)) ? BLK_FILE_BLOCK : BLK_CACHE_BLOCK;
  SBlockInfo blockInfo = getBlockBasicInfo(pRuntimeEnv, pBlock, blockType);

  // get the qualified data that can be skipped
  int32_t maxReads = (QUERY_IS_ASC_QUERY(pQuery)) ? blockInfo.size - pQuery->pos : pQuery->pos + 1;

  if (pQuery->limit.offset < maxReads || (pQuery->ekey <= blockInfo.keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
      (pQuery->ekey >= blockInfo.keyFirst && !QUERY_IS_ASC_QUERY(pQuery))) {  // start position in current block
    updateOffsetVal(pRuntimeEnv, &blockInfo, pBlock);
  } else {
    pQuery->limit.offset -= maxReads;
    // update the lastkey, since the following skip operation may traverse to another media. update the lastkey first.
    pQuery->lastKey = (QUERY_IS_ASC_QUERY(pQuery)) ? blockInfo.keyLast + 1 : blockInfo.keyFirst - 1;
    doSkipDataBlock(pRuntimeEnv);
  }
}

static bool forwardQueryStartPosIfNeeded(SQInfo *pQInfo, SMeterQuerySupportObj *pSupporter, bool dataInDisk,
                                         bool dataInCache) {
  SQuery *pQuery = &pQInfo->query;

  /* if queried with value filter, do NOT forward query start position */
  if (pQuery->numOfFilterCols > 0 || pSupporter->runtimeEnv.pTSBuf != NULL) {
    return true;
  }

  if (pQuery->limit.offset > 0 && (!isTopBottomQuery(pQuery)) && pQuery->interpoType == TSDB_INTERPO_NONE) {
    /*
     * 1. for top/bottom query, the offset applies to the final result, not here
     * 2. for interval without interpolation query we forward pQuery->nAggTimeInterval at a time for
     *    pQuery->limit.offset times. Since hole exists, pQuery->nAggTimeInterval*pQuery->limit.offset value is
     *    not valid. otherwise, we only forward pQuery->limit.offset number of points
     */
    if (pQuery->nAggTimeInterval > 0) {
      while (1) {
        /*
         * the skey may not be the aligned start time
         * 1. it is the value of first existed data point, therefore, the range
         *    between skey and ekey may be less than the interval value.
         * 2. the ekey may not be the actual end value of time interval, in case of the
         */
        if (QUERY_IS_ASC_QUERY(pQuery)) {
          pQuery->skey = pQuery->ekey + 1;
        } else {
          pQuery->skey = pQuery->ekey - 1;
        }

        // boundary check
        if ((pQuery->skey > pSupporter->rawEKey && QUERY_IS_ASC_QUERY(pQuery)) ||
            (pQuery->skey < pSupporter->rawEKey && !QUERY_IS_ASC_QUERY(pQuery))) {
          setQueryStatus(pQuery, QUERY_COMPLETED);

          sem_post(&pQInfo->dataReady);
          pQInfo->over = 1;
          return false;
        }

        /*
         * NOTE: the end key must be set the last value, to cover all possible
         * data. Otherwise, it may contain no data with only one interval time range
         */
        pQuery->ekey = pSupporter->rawEKey;
        pQuery->lastKey = pQuery->skey;

        // todo opt performance
        if (normalizedFirstQueryRange(dataInDisk, dataInCache, pSupporter, NULL, NULL) == false) {
          sem_post(&pQInfo->dataReady);  // hack for next read for empty return
          pQInfo->over = 1;
          return false;
        }

        if (--pQuery->limit.offset == 0) {
          break;
        }
      }
    } else {
      forwardQueryStartPosition(&pSupporter->runtimeEnv);
      if (Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK)) {
        setQueryStatus(pQuery, QUERY_COMPLETED);

        sem_post(&pQInfo->dataReady);  // hack for next read for empty return;
        pQInfo->over = 1;
        return false;
      }
    }
  }

  return true;
}

static void doSetInterpVal(SQLFunctionCtx *pCtx, TSKEY ts, int16_t type, int32_t index, char *data) {
  assert(pCtx->param[index].pz == NULL);

  int32_t len = 0;
  size_t  t = 0;

  if (type == TSDB_DATA_TYPE_BINARY) {
    t = strlen(data);

    len = t + 1 + TSDB_KEYSIZE;
    pCtx->param[index].pz = calloc(1, len);
  } else if (type == TSDB_DATA_TYPE_NCHAR) {
    t = wcslen((const wchar_t *)data);

    len = (t + 1) * TSDB_NCHAR_SIZE + TSDB_KEYSIZE;
    pCtx->param[index].pz = calloc(1, len);
  } else {
    len = TSDB_KEYSIZE * 2;
    pCtx->param[index].pz = malloc(len);
  }

  pCtx->param[index].nType = TSDB_DATA_TYPE_BINARY;

  char *z = pCtx->param[index].pz;
  *(TSKEY *)z = ts;
  z += TSDB_KEYSIZE;

  switch (type) {
    case TSDB_DATA_TYPE_FLOAT:
      *(double *)z = GET_FLOAT_VAL(data);
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      *(double *)z = GET_DOUBLE_VAL(data);
      break;
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_BOOL:
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT:
    case TSDB_DATA_TYPE_TIMESTAMP:
      *(int64_t *)z = GET_INT64_VAL(data);
      break;
    case TSDB_DATA_TYPE_BINARY:
      strncpy(z, data, t);
      break;
    case TSDB_DATA_TYPE_NCHAR: {
      wcsncpy((wchar_t *)z, (const wchar_t *)data, t);
    } break;
    default:
      assert(0);
  }

  pCtx->param[index].nLen = len;
}

/**
 * param[1]: default value/previous value of specified timestamp
 * param[2]: next value of specified timestamp
 * param[3]: denotes if the result is a precious result or interpolation results
 *
 * @param pQInfo
 * @param pSupporter
 * @param pInterpoRaw
 */
void pointInterpSupporterSetData(SQInfo *pQInfo, SPointInterpoSupporter *pPointInterpSupport) {
  // not point interpolation query, abort
  if (!isPointInterpoQuery(&pQInfo->query)) {
    return;
  }

  SQuery *               pQuery = &pQInfo->query;
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;
  SQueryRuntimeEnv *     pRuntimeEnv = &pSupporter->runtimeEnv;

  int32_t count = 1;
  TSKEY   key = *(TSKEY *)pPointInterpSupport->pNextPoint[0];

  if (key == pSupporter->rawSKey) {
    // the queried timestamp has value, return it directly without interpolation
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      tVariantCreateFromBinary(&pRuntimeEnv->pCtx[i].param[3], (char *)&count, sizeof(count), TSDB_DATA_TYPE_INT);

      pRuntimeEnv->pCtx[i].param[0].i64Key = key;
      pRuntimeEnv->pCtx[i].param[0].nType = TSDB_DATA_TYPE_BIGINT;
    }
  } else {
    // set the direct previous(next) point for process
    count = 2;

    if (pQuery->interpoType == TSDB_INTERPO_SET_VALUE) {
      for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
        SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];

        // only the function of interp needs the corresponding information
        if (pCtx->functionId != TSDB_FUNC_INTERP) {
          continue;
        }

        pCtx->numOfParams = 4;

        SInterpInfo *pInterpInfo = (SInterpInfo *)pRuntimeEnv->pCtx[i].aOutputBuf;
        pInterpInfo->pInterpDetail = calloc(1, sizeof(SInterpInfoDetail));

        SInterpInfoDetail *pInterpDetail = pInterpInfo->pInterpDetail;

        // for primary timestamp column, set the flag
        if (pQuery->pSelectExpr[i].pBase.colInfo.colId == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
          pInterpDetail->primaryCol = 1;
        }

        tVariantCreateFromBinary(&pCtx->param[3], (char *)&count, sizeof(count), TSDB_DATA_TYPE_INT);

        if (isNull((char *)&pQuery->defaultVal[i], pCtx->inputType)) {
          pCtx->param[1].nType = TSDB_DATA_TYPE_NULL;
        } else {
          tVariantCreateFromBinary(&pCtx->param[1], (char *)&pQuery->defaultVal[i], pCtx->inputBytes, pCtx->inputType);
        }

        pInterpDetail->ts = pSupporter->rawSKey;
        pInterpDetail->type = pQuery->interpoType;
      }
    } else {
      TSKEY prevKey = *(TSKEY *)pPointInterpSupport->pPrevPoint[0];
      TSKEY nextKey = *(TSKEY *)pPointInterpSupport->pNextPoint[0];

      for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
        SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];

        // tag column does not need the interp environment
        if (pQuery->pSelectExpr[i].pBase.functionId == TSDB_FUNC_TAG) {
          continue;
        }

        int32_t colInBuf = pQuery->pSelectExpr[i].pBase.colInfo.colIdxInBuf;

        SInterpInfo *pInterpInfo = (SInterpInfo *)pRuntimeEnv->pCtx[i].aOutputBuf;

        pInterpInfo->pInterpDetail = calloc(1, sizeof(SInterpInfoDetail));
        SInterpInfoDetail *pInterpDetail = pInterpInfo->pInterpDetail;

        int32_t type = GET_COLUMN_TYPE(pQuery, i);

        // for primary timestamp column, set the flag
        if (pQuery->pSelectExpr[i].pBase.colInfo.colId == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
          pInterpDetail->primaryCol = 1;
        } else {
          doSetInterpVal(pCtx, prevKey, type, 1, pPointInterpSupport->pPrevPoint[colInBuf]);
          doSetInterpVal(pCtx, nextKey, type, 2, pPointInterpSupport->pNextPoint[colInBuf]);
        }

        tVariantCreateFromBinary(&pRuntimeEnv->pCtx[i].param[3], (char *)&count, sizeof(count), TSDB_DATA_TYPE_INT);

        pInterpDetail->ts = pSupporter->rawSKey;
        pInterpDetail->type = pQuery->interpoType;
      }
    }
  }
}

void pointInterpSupporterInit(SQuery *pQuery, SPointInterpoSupporter *pInterpoSupport) {
  if (isPointInterpoQuery(pQuery)) {
    pInterpoSupport->pPrevPoint = malloc(pQuery->numOfCols * POINTER_BYTES);
    pInterpoSupport->pNextPoint = malloc(pQuery->numOfCols * POINTER_BYTES);

    pInterpoSupport->numOfCols = pQuery->numOfCols;

    /* get appropriated size for one row data source*/
    int32_t len = 0;
    for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
      len += pQuery->colList[i].data.bytes;
    }

    assert(PRIMARY_TSCOL_LOADED(pQuery));

    void *prev = calloc(1, len);
    void *next = calloc(1, len);

    int32_t offset = 0;

    for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
      pInterpoSupport->pPrevPoint[i] = prev + offset;
      pInterpoSupport->pNextPoint[i] = next + offset;

      offset += pQuery->colList[i].data.bytes;
    }
  }
}

void pointInterpSupporterDestroy(SPointInterpoSupporter *pPointInterpSupport) {
  if (pPointInterpSupport->numOfCols <= 0 || pPointInterpSupport->pPrevPoint == NULL) {
    return;
  }

  tfree(pPointInterpSupport->pPrevPoint[0]);
  tfree(pPointInterpSupport->pNextPoint[0]);

  tfree(pPointInterpSupport->pPrevPoint);
  tfree(pPointInterpSupport->pNextPoint);

  pPointInterpSupport->numOfCols = 0;
}

static void allocMemForInterpo(SMeterQuerySupportObj *pSupporter, SQuery *pQuery, SMeterObj *pMeterObj) {
  if (pQuery->interpoType != TSDB_INTERPO_NONE) {
    assert(pQuery->nAggTimeInterval > 0 || (pQuery->nAggTimeInterval == 0 && isPointInterpoQuery(pQuery)));

    if (pQuery->nAggTimeInterval > 0) {
      pSupporter->runtimeEnv.pInterpoBuf = malloc(POINTER_BYTES * pQuery->numOfOutputCols);

      for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
        pSupporter->runtimeEnv.pInterpoBuf[i] =
            calloc(1, sizeof(tFilePage) + pQuery->pSelectExpr[i].resBytes * pMeterObj->pointsPerFileBlock);
      }
    }
  }
}

static int32_t allocateOutputBufForGroup(SMeterQuerySupportObj *pSupporter, SQuery *pQuery, bool isMetricQuery) {
  int32_t slot = 0;

  if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    slot = 10000;
  } else {
    slot = pSupporter->pSidSet->numOfSubSet;
  }

  pSupporter->pResult = calloc(1, sizeof(SOutputRes) * slot);
  if (pSupporter->pResult == NULL) {
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  for (int32_t k = 0; k < slot; ++k) {
    SOutputRes *pOneRes = &pSupporter->pResult[k];
    pOneRes->nAlloc = 1;

    /*
     * for single table top/bottom query, the output for group by normal column, the output rows is
     * equals to the maximum rows, instead of 1.
     */
    if (!isMetricQuery && isTopBottomQuery(pQuery)) {
      assert(pQuery->numOfOutputCols > 1);

      SSqlFunctionExpr *pExpr = &pQuery->pSelectExpr[1];
      pOneRes->nAlloc = pExpr->pBase.arg[0].argValue.i64;
    }

    createGroupResultBuf(pQuery, pOneRes, isMetricQuery);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t allocateRuntimeEnvBuf(SQueryRuntimeEnv *pRuntimeEnv, SMeterObj *pMeterObj) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  // To make sure the start position of each buffer is aligned to 4bytes in 32-bit ARM system.
  for (int32_t i = 0; i < pQuery->numOfCols; ++i) {
    int32_t bytes = pQuery->colList[i].data.bytes;
    pRuntimeEnv->colDataBuffer[i] = calloc(1, sizeof(SData) + EXTRA_BYTES + pMeterObj->pointsPerFileBlock * bytes);
    if (pRuntimeEnv->colDataBuffer[i] == NULL) {
      goto _error_clean;
    }
  }

  // record the maximum column width among columns of this meter/metric
  int32_t maxColWidth = pQuery->colList[0].data.bytes;
  for (int32_t i = 1; i < pQuery->numOfCols; ++i) {
    int32_t bytes = pQuery->colList[i].data.bytes;
    if (bytes > maxColWidth) {
      maxColWidth = bytes;
    }
  }

  pRuntimeEnv->primaryColBuffer = NULL;
  if (PRIMARY_TSCOL_LOADED(pQuery)) {
    pRuntimeEnv->primaryColBuffer = pRuntimeEnv->colDataBuffer[0];
  } else {
    pRuntimeEnv->primaryColBuffer =
        (SData *)malloc(pMeterObj->pointsPerFileBlock * TSDB_KEYSIZE + sizeof(SData) + EXTRA_BYTES);
  }

  pRuntimeEnv->unzipBufSize = (size_t)(maxColWidth * pMeterObj->pointsPerFileBlock + EXTRA_BYTES);  // plus extra_bytes

  pRuntimeEnv->unzipBuffer = (char *)calloc(1, pRuntimeEnv->unzipBufSize);
  pRuntimeEnv->secondaryUnzipBuffer = (char *)calloc(1, pRuntimeEnv->unzipBufSize);

  if (pRuntimeEnv->unzipBuffer == NULL || pRuntimeEnv->secondaryUnzipBuffer == NULL ||
      pRuntimeEnv->primaryColBuffer == NULL) {
    goto _error_clean;
  }

  return TSDB_CODE_SUCCESS;

_error_clean:
  for (int32_t i = 0; i < pRuntimeEnv->pQuery->numOfCols; ++i) {
    tfree(pRuntimeEnv->colDataBuffer[i]);
  }

  tfree(pRuntimeEnv->unzipBuffer);
  tfree(pRuntimeEnv->secondaryUnzipBuffer);

  if (!PRIMARY_TSCOL_LOADED(pQuery)) {
    tfree(pRuntimeEnv->primaryColBuffer);
  }

  return TSDB_CODE_SERV_OUT_OF_MEMORY;
}

int32_t vnodeQuerySingleMeterPrepare(SQInfo *pQInfo, SMeterObj *pMeterObj, SMeterQuerySupportObj *pSupporter,
                                     void *param) {
  SQuery *pQuery = &pQInfo->query;
  int32_t code = TSDB_CODE_SUCCESS;

  /*
   * only the successful complete requries the sem_post/over = 1 operations.
   */
  if ((QUERY_IS_ASC_QUERY(pQuery) && (pQuery->skey > pQuery->ekey)) ||
      (!QUERY_IS_ASC_QUERY(pQuery) && (pQuery->ekey > pQuery->skey))) {
    dTrace("QInfo:%p no result in time range %" PRId64 "-%" PRId64 ", order %d", pQInfo, pQuery->skey, pQuery->ekey,
           pQuery->order.order);

    sem_post(&pQInfo->dataReady);
    pQInfo->over = 1;
    return TSDB_CODE_SUCCESS;
  }

  setScanLimitationByResultBuffer(pQuery);
  changeExecuteScanOrder(pQuery, false);

  pQInfo->over = 0;
  pQInfo->pointsRead = 0;
  pQuery->pointsRead = 0;

  // dataInCache requires lastKey value
  pQuery->lastKey = pQuery->skey;

  doInitQueryFileInfoFD(&pSupporter->runtimeEnv.vnodeFileInfo);

  vnodeInitDataBlockInfo(&pSupporter->runtimeEnv.loadBlockInfo);
  vnodeInitLoadCompBlockInfo(&pSupporter->runtimeEnv.loadCompBlockInfo);

  // check data in file or cache
  bool dataInCache = true;
  bool dataInDisk = true;

  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  pRuntimeEnv->pQuery = pQuery;
  pRuntimeEnv->pMeterObj = pMeterObj;

  if ((code = allocateRuntimeEnvBuf(pRuntimeEnv, pMeterObj)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  vnodeCheckIfDataExists(pRuntimeEnv, pMeterObj, &dataInDisk, &dataInCache);

  /* data in file or cache is not qualified for the query. abort */
  if (!(dataInCache || dataInDisk)) {
    dTrace("QInfo:%p no result in query", pQInfo);
    sem_post(&pQInfo->dataReady);
    pQInfo->over = 1;
    return code;
  }

  pRuntimeEnv->pTSBuf = param;
  pRuntimeEnv->cur.vnodeIndex = -1;
  if (param != NULL) {
    int16_t order = (pQuery->order.order == pRuntimeEnv->pTSBuf->tsOrder) ? TSQL_SO_ASC : TSQL_SO_DESC;
    tsBufSetTraverseOrder(pRuntimeEnv->pTSBuf, order);
  }

  // create runtime environment
  code = setupQueryRuntimeEnv(pMeterObj, pQuery, &pSupporter->runtimeEnv, NULL, pQuery->order.order, false);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  vnodeRecordAllFiles(pQInfo, pMeterObj->vnode);

  if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    if ((code = allocateOutputBufForGroup(pSupporter, pQuery, false)) != TSDB_CODE_SUCCESS) {
      return code;
    }

    int16_t type = TSDB_DATA_TYPE_NULL;
    if (isGroupbyNormalCol(pQuery->pGroupbyExpr)) {
      type = getGroupbyColumnType(pQuery, pQuery->pGroupbyExpr);
    } else {
      type = TSDB_DATA_TYPE_TIMESTAMP;
    }

    // todo bug!
    initSlidingWindowInfo(&pRuntimeEnv->swindowResInfo, 3, type, pSupporter->pResult);
  }

  pSupporter->rawSKey = pQuery->skey;
  pSupporter->rawEKey = pQuery->ekey;

  /* query on single table */
  pSupporter->numOfMeters = 1;
  setQueryStatus(pQuery, QUERY_NOT_COMPLETED);

  SPointInterpoSupporter interpInfo = {0};
  pointInterpSupporterInit(pQuery, &interpInfo);

  /*
   * in case of last_row query without query range, we set the query timestamp to
   * pMeterObj->lastKey. Otherwise, keep the initial query time range unchanged.
   */

  if (isFirstLastRowQuery(pQuery) && notHasQueryTimeRange(pQuery)) {
    if (!normalizeUnBoundLastRowQuery(pSupporter, &interpInfo)) {
      sem_post(&pQInfo->dataReady);
      pQInfo->over = 1;

      pointInterpSupporterDestroy(&interpInfo);
      return TSDB_CODE_SUCCESS;
    }
  } else {
    // find the skey and ekey in case of sliding query
    // todo refactor
    if (pQuery->slidingTime > 0 && pQuery->nAggTimeInterval > 0) {
      int64_t skey = 0;

      SWAP(pQuery->skey, pQuery->ekey, int64_t);
      pQuery->order.order ^= 1;
      pQuery->lastKey = pQuery->skey;

      if (normalizedFirstQueryRange(dataInDisk, dataInCache, pSupporter, &interpInfo, &skey) == false) {
        sem_post(&pQInfo->dataReady);
        pQInfo->over = 1;

        pointInterpSupporterDestroy(&interpInfo);
        return TSDB_CODE_SUCCESS;
      }

      pQuery->skey = skey;

      pQuery->order.order ^= 1;
      SWAP(pQuery->skey, pQuery->ekey, int64_t);

      int64_t ekey = 0;
      pQuery->lastKey = pQuery->skey;
      if (normalizedFirstQueryRange(dataInDisk, dataInCache, pSupporter, &interpInfo, &ekey) == false) {
        //
      }

      pQuery->skey = ekey;

      TSKEY skey1, ekey1;
      TSKEY windowSKey = 0, windowEKey = 0;

      TSKEY minKey = MIN(pQuery->skey, pQuery->ekey);
      TSKEY maxKey = MAX(pQuery->skey, pQuery->ekey);

      doGetAlignedIntervalQueryRangeImpl(pQuery, minKey, minKey, maxKey, &skey1, &ekey1, &windowSKey, &windowEKey);
      pRuntimeEnv->swindowResInfo.startTime = windowSKey;

      pSupporter->rawSKey = pQuery->skey;
      pSupporter->rawEKey = pQuery->ekey;

      if (QUERY_IS_ASC_QUERY(pQuery)) {
        pRuntimeEnv->swindowResInfo.prevSKey = windowSKey;
      } else {
        pRuntimeEnv->swindowResInfo.prevSKey = windowSKey + ((pQuery->skey - windowSKey) / pQuery->slidingTime) * pQuery->slidingTime;
      }
    } else {
      int64_t ekey = 0;
      if ((normalizedFirstQueryRange(dataInDisk, dataInCache, pSupporter, &interpInfo, &ekey) == false) ||
          (isFixedOutputQuery(pQuery) && !isTopBottomQuery(pQuery) && (pQuery->limit.offset > 0)) ||
          (isTopBottomQuery(pQuery) && pQuery->limit.offset >= pQuery->pSelectExpr[1].pBase.arg[0].argValue.i64)) {
        sem_post(&pQInfo->dataReady);
        pQInfo->over = 1;

        pointInterpSupporterDestroy(&interpInfo);
        return TSDB_CODE_SUCCESS;
      }
    }
  }

  /*
   * here we set the value for before and after the specified time into the
   * parameter for interpolation query
   */
  pointInterpSupporterSetData(pQInfo, &interpInfo);
  pointInterpSupporterDestroy(&interpInfo);

  if (!forwardQueryStartPosIfNeeded(pQInfo, pSupporter, dataInDisk, dataInCache)) {
    return TSDB_CODE_SUCCESS;
  }

  int64_t rs = taosGetIntervalStartTimestamp(pSupporter->rawSKey, pQuery->nAggTimeInterval, pQuery->intervalTimeUnit,
                                             pQuery->precision);
  taosInitInterpoInfo(&pRuntimeEnv->interpoInfo, pQuery->order.order, rs, 0, 0);
  allocMemForInterpo(pSupporter, pQuery, pMeterObj);

  if (!isPointInterpoQuery(pQuery)) {
    assert(pQuery->pos >= 0 && pQuery->slot >= 0);
  }

  // the pQuery->skey is changed during normalizedFirstQueryRange, so set the newest lastkey value
  pQuery->lastKey = pQuery->skey;
  return TSDB_CODE_SUCCESS;
}

void vnodeQueryFreeQInfoEx(SQInfo *pQInfo) {
  if (pQInfo == NULL || pQInfo->pMeterQuerySupporter == NULL) {
    return;
  }

  SQuery *               pQuery = &pQInfo->query;
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;

  teardownQueryRuntimeEnv(&pSupporter->runtimeEnv);
  tfree(pSupporter->pMeterSidExtInfo);

  if (pSupporter->pMetersHashTable != NULL) {
    taosCleanUpHashTable(pSupporter->pMetersHashTable);
    pSupporter->pMetersHashTable = NULL;
  }

  if (pSupporter->pSidSet != NULL || isGroupbyNormalCol(pQInfo->query.pGroupbyExpr) ||
      (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    int32_t size = 0;
    if (isGroupbyNormalCol(pQInfo->query.pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
      size = 10000;
    } else if (pSupporter->pSidSet != NULL) {
      size = pSupporter->pSidSet->numOfSubSet;
    }

    for (int32_t i = 0; i < size; ++i) {
      destroyGroupResultBuf(&pSupporter->pResult[i], pQInfo->query.numOfOutputCols);
    }
  }

  if (FD_VALID(pSupporter->meterOutputFd)) {
    assert(pSupporter->meterOutputMMapBuf != NULL);
    dTrace("QInfo:%p disk-based output buffer during query:%" PRId64 " bytes", pQInfo, pSupporter->bufSize);
    munmap(pSupporter->meterOutputMMapBuf, pSupporter->bufSize);
    tclose(pSupporter->meterOutputFd);

    unlink(pSupporter->extBufFile);
  }

  tSidSetDestroy(&pSupporter->pSidSet);

  if (pSupporter->pMeterDataInfo != NULL) {
    for (int32_t j = 0; j < pSupporter->numOfMeters; ++j) {
      destroyMeterQueryInfo(pSupporter->pMeterDataInfo[j].pMeterQInfo, pQuery->numOfOutputCols);
      free(pSupporter->pMeterDataInfo[j].pBlock);
    }
  }

  tfree(pSupporter->pMeterDataInfo);

  tfree(pSupporter->pResult);
  tfree(pQInfo->pMeterQuerySupporter);
}

int32_t vnodeMultiMeterQueryPrepare(SQInfo *pQInfo, SQuery *pQuery, void *param) {
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;

  if ((QUERY_IS_ASC_QUERY(pQuery) && (pQuery->skey > pQuery->ekey)) ||
      (!QUERY_IS_ASC_QUERY(pQuery) && (pQuery->ekey > pQuery->skey))) {
    dTrace("QInfo:%p no result in time range %" PRId64 "-%" PRId64 ", order %d", pQInfo, pQuery->skey, pQuery->ekey,
           pQuery->order.order);

    sem_post(&pQInfo->dataReady);
    pQInfo->over = 1;

    return TSDB_CODE_SUCCESS;
  }

  pQInfo->over = 0;
  pQInfo->pointsRead = 0;
  pQuery->pointsRead = 0;

  changeExecuteScanOrder(pQuery, true);
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;

  doInitQueryFileInfoFD(&pRuntimeEnv->vnodeFileInfo);
  vnodeInitDataBlockInfo(&pRuntimeEnv->loadBlockInfo);
  vnodeInitLoadCompBlockInfo(&pRuntimeEnv->loadCompBlockInfo);

  /*
   * since we employ the output control mechanism in main loop.
   * so, disable it during data block scan procedure.
   */
  setScanLimitationByResultBuffer(pQuery);

  // save raw query range for applying to each subgroup
  pSupporter->rawEKey = pQuery->ekey;
  pSupporter->rawSKey = pQuery->skey;
  pQuery->lastKey = pQuery->skey;

  // create runtime environment
  SSchema *pTagSchema = NULL;

  tTagSchema *pTagSchemaInfo = pSupporter->pSidSet->pTagSchema;
  if (pTagSchemaInfo != NULL) {
    pTagSchema = pTagSchemaInfo->pSchema;
  }

  // get one queried meter
  SMeterObj *pMeter = getMeterObj(pSupporter->pMetersHashTable, pSupporter->pSidSet->pSids[0]->sid);

  pRuntimeEnv->pTSBuf = param;
  pRuntimeEnv->cur.vnodeIndex = -1;

  // set the ts-comp file traverse order
  if (param != NULL) {
    int16_t order = (pQuery->order.order == pRuntimeEnv->pTSBuf->tsOrder) ? TSQL_SO_ASC : TSQL_SO_DESC;
    tsBufSetTraverseOrder(pRuntimeEnv->pTSBuf, order);
  }

  int32_t ret = setupQueryRuntimeEnv(pMeter, pQuery, &pSupporter->runtimeEnv, pTagSchema, TSQL_SO_ASC, true);
  if (ret != TSDB_CODE_SUCCESS) {
    return ret;
  }

  ret = allocateRuntimeEnvBuf(pRuntimeEnv, pMeter);
  if (ret != TSDB_CODE_SUCCESS) {
    return ret;
  }

  tSidSetSort(pSupporter->pSidSet);
  vnodeRecordAllFiles(pQInfo, pMeter->vnode);

  if ((ret = allocateOutputBufForGroup(pSupporter, pQuery, true)) != TSDB_CODE_SUCCESS) {
    return ret;
  }

  if (isGroupbyNormalCol(pQuery->pGroupbyExpr)) {  // group by columns not tags;
    int16_t type = getGroupbyColumnType(pQuery, pQuery->pGroupbyExpr);
    initSlidingWindowInfo(&pRuntimeEnv->swindowResInfo, 10039, type, pSupporter->pResult);
  }

  if (pQuery->nAggTimeInterval != 0 || isSumAvgRateQuery(pQuery)) {
    getTmpfilePath("tb_metric_mmap", pSupporter->extBufFile);
    pSupporter->meterOutputFd = open(pSupporter->extBufFile, O_CREAT | O_RDWR, 0666);

    if (!FD_VALID(pSupporter->meterOutputFd)) {
      dError("QInfo:%p failed to create file: %s on disk. %s", pQInfo, pSupporter->extBufFile, strerror(errno));
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }

    pSupporter->numOfPages = pSupporter->numOfMeters;

    ret = ftruncate(pSupporter->meterOutputFd, pSupporter->numOfPages * DEFAULT_INTERN_BUF_SIZE);
    if (ret != TSDB_CODE_SUCCESS) {
      dError("QInfo:%p failed to create intermediate result output file:%s. %s", pQInfo, pSupporter->extBufFile,
             strerror(errno));
      return TSDB_CODE_SERV_NO_DISKSPACE;
    }

    pRuntimeEnv->numOfRowsPerPage = (DEFAULT_INTERN_BUF_SIZE - sizeof(tFilePage)) / pQuery->rowSize;
    pSupporter->lastPageId = -1;
    pSupporter->bufSize = pSupporter->numOfPages * DEFAULT_INTERN_BUF_SIZE;

    pSupporter->meterOutputMMapBuf =
        mmap(NULL, pSupporter->bufSize, PROT_READ | PROT_WRITE, MAP_SHARED, pSupporter->meterOutputFd, 0);
    if (pSupporter->meterOutputMMapBuf == MAP_FAILED) {
      dError("QInfo:%p failed to map temp file: %s. %s", pQInfo, pSupporter->extBufFile, strerror(errno));
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }
  }

  // metric query do not invoke interpolation, it will be done at the second-stage merge
  if (!isPointInterpoQuery(pQuery)) {
    pQuery->interpoType = TSDB_INTERPO_NONE;
  }

  TSKEY revisedStime = taosGetIntervalStartTimestamp(pSupporter->rawSKey, pQuery->nAggTimeInterval,
                                                     pQuery->intervalTimeUnit, pQuery->precision);
  taosInitInterpoInfo(&pRuntimeEnv->interpoInfo, pQuery->order.order, revisedStime, 0, 0);

  return TSDB_CODE_SUCCESS;
}

/**
 * decrease the refcount for each table involved in this query
 * @param pQInfo
 */
void vnodeDecMeterRefcnt(SQInfo *pQInfo) {
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;

  if (pSupporter == NULL || pSupporter->numOfMeters == 1) {
    atomic_fetch_sub_32(&pQInfo->pObj->numOfQueries, 1);
    dTrace("QInfo:%p vid:%d sid:%d meterId:%s, query is over, numOfQueries:%d", pQInfo, pQInfo->pObj->vnode,
           pQInfo->pObj->sid, pQInfo->pObj->meterId, pQInfo->pObj->numOfQueries);
  } else {
    int32_t num = 0;
    for (int32_t i = 0; i < pSupporter->numOfMeters; ++i) {
      SMeterObj *pMeter = getMeterObj(pSupporter->pMetersHashTable, pSupporter->pSidSet->pSids[i]->sid);
      atomic_fetch_sub_32(&(pMeter->numOfQueries), 1);

      if (pMeter->numOfQueries > 0) {
        dTrace("QInfo:%p vid:%d sid:%d meterId:%s, query is over, numOfQueries:%d", pQInfo, pMeter->vnode, pMeter->sid,
               pMeter->meterId, pMeter->numOfQueries);
        num++;
      }
    }

    /*
     * in order to reduce log output, for all meters of which numOfQueries count are 0,
     * we do not output corresponding information
     */
    num = pSupporter->numOfMeters - num;
    dTrace("QInfo:%p metric query is over, dec query ref for %d meters, numOfQueries on %d meters are 0", pQInfo,
           pSupporter->numOfMeters, num);
  }
}

TSKEY getTimestampInCacheBlock(SQueryRuntimeEnv *pRuntimeEnv, SCacheBlock *pBlock, int32_t index) {
  if (pBlock == NULL || index >= pBlock->numOfPoints || index < 0) {
    return -1;
  }

  return ((TSKEY *)(pRuntimeEnv->primaryColBuffer->data))[index];
}

/*
 * NOTE: pQuery->pos will not change, the corresponding data block will be loaded into buffer
 * loadDataBlockOnDemand will change the value of pQuery->pos, according to the pQuery->lastKey
 */
TSKEY getTimestampInDiskBlock(SQueryRuntimeEnv *pRuntimeEnv, int32_t index) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  /*
   * the corresponding compblock info has been loaded already
   * todo add check for compblock loaded
   */
  SCompBlock *pBlock = getDiskDataBlock(pQuery, pQuery->slot);

  // this block must be loaded into buffer
  SLoadDataBlockInfo *pLoadInfo = &pRuntimeEnv->loadBlockInfo;
  assert(pQuery->pos >= 0 && pQuery->pos < pBlock->numOfPoints);

  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  int32_t fileIndex = vnodeGetVnodeHeaderFileIdx(&pQuery->fileId, pRuntimeEnv, pQuery->order.order);

  dTrace("QInfo:%p vid:%d sid:%d id:%s, fileId:%d, slot:%d load data block due to primary key required",
         GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, pQuery->slot);

  bool    loadTS = true;
  bool    loadFields = true;
  int32_t slot = pQuery->slot;

  int32_t ret = loadDataBlockIntoMem(pBlock, &pQuery->pFields[slot], pRuntimeEnv, fileIndex, loadTS, loadFields);
  if (ret != TSDB_CODE_SUCCESS) {
    return -1;
  }

  SET_DATA_BLOCK_LOADED(pRuntimeEnv->blockStatus);
  SET_FILE_BLOCK_FLAG(pRuntimeEnv->blockStatus);

  assert(pQuery->fileId == pLoadInfo->fileId && pQuery->slot == pLoadInfo->slotIdx);
  return ((TSKEY *)pRuntimeEnv->primaryColBuffer->data)[index];
}

// todo remove this function
static TSKEY getFirstDataBlockInCache(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  assert(pQuery->fileId == -1 && QUERY_IS_ASC_QUERY(pQuery));

  /*
   * get the start position in cache according to the pQuery->lastkey
   *
   * In case of cache and disk file data overlaps and all required data are commit to disk file,
   * there are no qualified data available in cache, we need to set the QUERY_COMPLETED flag.
   *
   * If cache data and disk-based data are not completely overlapped, cacheBoundaryCheck function will set the
   * correct status flag.
   */
  TSKEY nextTimestamp = getQueryStartPositionInCache(pRuntimeEnv, &pQuery->slot, &pQuery->pos, true);
  if (nextTimestamp < 0) {
    setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
  } else if (nextTimestamp > pQuery->ekey) {
    setQueryStatus(pQuery, QUERY_COMPLETED);
  }

  return nextTimestamp;
}

TSKEY getQueryPositionForCacheInvalid(SQueryRuntimeEnv *pRuntimeEnv, __block_search_fn_t searchFn) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SQInfo *   pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;
  int32_t    step = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  dTrace(
      "QInfo:%p vid:%d sid:%d id:%s cache block re-allocated to other meter, "
      "try get query start position in file/cache, qrange:%" PRId64 "-%" PRId64 ", lastKey:%" PRId64,
      pQInfo, pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->skey, pQuery->ekey, pQuery->lastKey);

  if (step == QUERY_DESC_FORWARD_STEP) {
    /*
     * In descending order query, if the cache is invalid, it must be flushed to disk.
     * Try to find the appropriate position in file, and no need to search cache any more.
     */
    bool ret = getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_LESS_EQUAL, searchFn);

    dTrace("QInfo:%p vid:%d sid:%d id:%s find the possible position in file, fileId:%d, slot:%d, pos:%d", pQInfo,
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, pQuery->slot, pQuery->pos);

    if (ret) {
      TSKEY key = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);

      // key in query range. If not, no qualified in disk file
      if (key < pQuery->ekey) {
        setQueryStatus(pQuery, QUERY_COMPLETED);
      }

      return key;
    } else {
      setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
      return -1;  // no data to check
    }
  } else {  // asc query
    bool ret = getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_GREATER_EQUAL, searchFn);
    if (ret) {
      dTrace("QInfo:%p vid:%d sid:%d id:%s find the possible position, fileId:%d, slot:%d, pos:%d", pQInfo,
             pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, pQuery->slot, pQuery->pos);

      TSKEY key = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);

      // key in query range. If not, no qualified in disk file
      if (key > pQuery->ekey) {
        setQueryStatus(pQuery, QUERY_COMPLETED);
      }

      return key;
    } else {
      /*
       * all data in file is less than the pQuery->lastKey, try cache again.
       * cache block status will be set in getFirstDataBlockInCache function
       */
      TSKEY key = getFirstDataBlockInCache(pRuntimeEnv);

      dTrace("QInfo:%p vid:%d sid:%d id:%s find the new position in cache, fileId:%d, slot:%d, pos:%d", pQInfo,
             pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->fileId, pQuery->slot, pQuery->pos);
      return key;
    }
  }
}

static int32_t moveToNextBlockInCache(SQueryRuntimeEnv *pRuntimeEnv, int32_t step, __block_search_fn_t searchFn) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  SCacheInfo *pCacheInfo = (SCacheInfo *)pMeterObj->pCache;
  assert(pQuery->fileId < 0);

  /*
   * ascending order to last cache block all data block in cache have been iterated, no need to set
   * pRuntimeEnv->nextPos. done
   */
  if (step == QUERY_ASC_FORWARD_STEP && pQuery->slot == pQuery->currentSlot) {
    setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
    return DISK_DATA_LOADED;
  }

  /*
   * descending order to first cache block, try file
   * NOTE: use the real time cache information, not the snapshot
   */
  int32_t numOfBlocks = pCacheInfo->numOfBlocks;
  int32_t currentSlot = pCacheInfo->currentSlot;

  int32_t firstSlot = getFirstCacheSlot(numOfBlocks, currentSlot, pCacheInfo);

  if (step == QUERY_DESC_FORWARD_STEP && pQuery->slot == firstSlot) {
    bool ret = getQualifiedDataBlock(pMeterObj, pRuntimeEnv, QUERY_RANGE_LESS_EQUAL, searchFn);
    if (ret) {
      TSKEY key = getTimestampInDiskBlock(pRuntimeEnv, pQuery->pos);

      // key in query range. If not, no qualified in disk file
      if (key < pQuery->ekey) {
        setQueryStatus(pQuery, QUERY_COMPLETED);
      }

      // the skip operation does NOT set the startPos yet
      //      assert(pRuntimeEnv->startPos.fileId < 0);
    } else {
      setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
    }
    return DISK_DATA_LOADED;
  }

  /* now still iterate the cache data blocks */
  pQuery->slot = (pQuery->slot + step + pCacheInfo->maxBlocks) % pCacheInfo->maxBlocks;
  SCacheBlock *pBlock = getCacheDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);

  /*
   * data in this cache block has been flushed to disk, then we should locate the start position in file.
   * In both desc/asc query, this situation may occur. And we need to locate the start query position in file or cache.
   */
  if (pBlock == NULL) {
    getQueryPositionForCacheInvalid(pRuntimeEnv, searchFn);

    return DISK_DATA_LOADED;
  } else {
    pQuery->pos = (QUERY_IS_ASC_QUERY(pQuery)) ? 0 : pBlock->numOfPoints - 1;

    TSKEY startkey = getTimestampInCacheBlock(pRuntimeEnv, pBlock, pQuery->pos);
    if (startkey < 0) {
      setQueryStatus(pQuery, QUERY_COMPLETED);
    }

    SET_CACHE_BLOCK_FLAG(pRuntimeEnv->blockStatus);

    dTrace("QInfo:%p check cache block, blockId:%d slot:%d pos:%d, blockstatus:%d", GET_QINFO_ADDR(pQuery),
           pQuery->blockId, pQuery->slot, pQuery->pos, pRuntimeEnv->blockStatus);
  }

  return DISK_DATA_LOADED;
}

/**
 *  move the cursor to next block and not load
 */
static int32_t moveToNextBlock(SQueryRuntimeEnv *pRuntimeEnv, int32_t step, __block_search_fn_t searchFn,
                               bool loadData) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  SET_DATA_BLOCK_NOT_LOADED(pRuntimeEnv->blockStatus);

  if (pQuery->fileId >= 0) {
    int32_t fileIndex = -1;

    /*
     * 1. ascending  order. The last data block of data file
     * 2. descending order. The first block of file
     */
    if ((step == QUERY_ASC_FORWARD_STEP && (pQuery->slot == pQuery->numOfBlocks - 1)) ||
        (step == QUERY_DESC_FORWARD_STEP && (pQuery->slot == 0))) {
      fileIndex = getNextDataFileCompInfo(pRuntimeEnv, pMeterObj, step);
      /* data maybe in cache */

      if (fileIndex >= 0) {  // next file
        pQuery->slot = (step == QUERY_ASC_FORWARD_STEP) ? 0 : pQuery->numOfBlocks - 1;
        pQuery->pos = (step == QUERY_ASC_FORWARD_STEP) ? 0 : pQuery->pBlock[pQuery->slot].numOfPoints - 1;
      } else {  // try data in cache
        assert(pQuery->fileId == -1);

        if (step == QUERY_ASC_FORWARD_STEP) {
          getFirstDataBlockInCache(pRuntimeEnv);
        } else {  // no data to check for desc order query
          setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
        }

        return DISK_DATA_LOADED;
      }
    } else {  // next block in the same file
      int32_t fid = pQuery->fileId;
      fileIndex = vnodeGetVnodeHeaderFileIdx(&fid, pRuntimeEnv, pQuery->order.order);

      pQuery->slot += step;
      pQuery->pos = (step == QUERY_ASC_FORWARD_STEP) ? 0 : pQuery->pBlock[pQuery->slot].numOfPoints - 1;
    }

    assert(pQuery->pBlock != NULL);

    /* no need to load data, return directly */
    if (!loadData) {
      return DISK_DATA_LOADED;
    }

    // load data block function will change the value of pQuery->pos
    int32_t ret =
        LoadDatablockOnDemand(&pQuery->pBlock[pQuery->slot], &pQuery->pFields[pQuery->slot], &pRuntimeEnv->blockStatus,
                              pRuntimeEnv, fileIndex, pQuery->slot, searchFn, true);
    if (ret != DISK_DATA_LOADED) {
      return ret;
    }
  } else {  // data in cache
    return moveToNextBlockInCache(pRuntimeEnv, step, searchFn);
  }

  return DISK_DATA_LOADED;
}

static void doHandleDataBlockImpl(SQueryRuntimeEnv *pRuntimeEnv, SBlockInfo *pblockInfo, __block_search_fn_t searchFn,
                                  int32_t *numOfRes, int32_t blockLoadStatus, int32_t *forwardStep) {
  SQuery *           pQuery = pRuntimeEnv->pQuery;
  SQueryCostSummary *pSummary = &pRuntimeEnv->summary;

  TSKEY * primaryKeys = (TSKEY *)pRuntimeEnv->primaryColBuffer->data;
  int64_t start = taosGetTimestampUs();

  if (IS_DISK_DATA_BLOCK(pQuery)) {
    SCompBlock *pBlock = getDiskDataBlock(pQuery, pQuery->slot);
    *pblockInfo = getBlockBasicInfo(pRuntimeEnv, pBlock, BLK_FILE_BLOCK);

    if (blockLoadStatus == DISK_DATA_LOADED) {
      *forwardStep = applyFunctionsOnBlock(pRuntimeEnv, pblockInfo, primaryKeys, pQuery->pFields[pQuery->slot],
                                           searchFn, numOfRes);
    } else {
      *forwardStep = pblockInfo->size;
    }

    pSummary->fileTimeUs += (taosGetTimestampUs() - start);
  } else {
    assert(vnodeIsDatablockLoaded(pRuntimeEnv, pRuntimeEnv->pMeterObj, -1, true) == DISK_BLOCK_NO_NEED_TO_LOAD);

    SCacheBlock *pBlock = getCacheDataBlock(pRuntimeEnv->pMeterObj, pRuntimeEnv, pQuery->slot);
    *pblockInfo = getBlockBasicInfo(pRuntimeEnv, pBlock, BLK_CACHE_BLOCK);

    *forwardStep = applyFunctionsOnBlock(pRuntimeEnv, pblockInfo, primaryKeys, NULL, searchFn, numOfRes);

    pSummary->cacheTimeUs += (taosGetTimestampUs() - start);
  }
}

static void getNextLogicalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, STimeWindow* pTimeWindow) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  int32_t factor = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);
  
  pTimeWindow->skey += (pQuery->slidingTime * factor);
  pTimeWindow->ekey += (pQuery->slidingTime * factor);
}

static int64_t doScanAllDataBlocks(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  bool    LOAD_DATA = true;

  int32_t forwardStep = 0;
  int64_t cnt = 0;

  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  __block_search_fn_t searchFn = vnodeSearchKeyFunc[pMeterObj->searchAlgorithm];
  int32_t             blockLoadStatus = DISK_DATA_LOADED;
  SQueryCostSummary * pSummary = &pRuntimeEnv->summary;

  int32_t step = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  // initial data block always be loaded
  SPositionInfo *pStartPos = &pRuntimeEnv->startPos;
  assert(pQuery->slot == pStartPos->slot);

  dTrace("QInfo:%p query start, qrange:%" PRId64 "-%" PRId64 ", lastkey:%" PRId64
         ", order:%d, start fileId:%d, slot:%d, pos:%d, bstatus:%d",
         GET_QINFO_ADDR(pQuery), pQuery->skey, pQuery->ekey, pQuery->lastKey, pQuery->order.order, pStartPos->fileId,
         pStartPos->slot, pStartPos->pos, pRuntimeEnv->blockStatus);

  while (1) {
    // check if query is killed or not set the status of query to pass the status check
    if (isQueryKilled(pQuery)) {
      setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
      return cnt;
    }

    int32_t    numOfRes = 0;
    SBlockInfo blockInfo = {0};
    doHandleDataBlockImpl(pRuntimeEnv, &blockInfo, searchFn, &numOfRes, blockLoadStatus, &forwardStep);

    dTrace("QInfo:%p check data block, brange:%" PRId64 "-%" PRId64
           ", fileId:%d, slot:%d, pos:%d, bstatus:%d, rows:%d, checked:%d",
           GET_QINFO_ADDR(pQuery), blockInfo.keyFirst, blockInfo.keyLast, pQuery->fileId, pQuery->slot, pQuery->pos,
           pRuntimeEnv->blockStatus, blockInfo.size, forwardStep);

    // save last access position
    int32_t accessPos = pQuery->pos + (forwardStep - 1) * step;
    savePointPosition(&pRuntimeEnv->endPos, pQuery->fileId, pQuery->slot, accessPos);

    cnt += forwardStep;

    if (queryCompleteInBlock(pQuery, &blockInfo, forwardStep)) {
      int32_t nextPos = accessPos + step;

      /*
       * set the next access position, nextPos only required by
       * 1. interval query.
       * 2. multi-output query that may cause buffer overflow.
       */
      if (pQuery->nAggTimeInterval > 0 ||
          (Q_STATUS_EQUAL(pQuery->over, QUERY_RESBUF_FULL) && pQuery->checkBufferInLoop == 1)) {
        if (nextPos >= blockInfo.size || nextPos < 0) {
          moveToNextBlock(pRuntimeEnv, step, searchFn, !LOAD_DATA);

          // slot/pos/fileId is updated in moveToNextBlock function
          savePointPosition(&pRuntimeEnv->nextPos, pQuery->fileId, pQuery->slot, pQuery->pos);
        } else {
          savePointPosition(&pRuntimeEnv->nextPos, pQuery->fileId, pQuery->slot, accessPos + step);
        }
      }
      break;
    } else {  // query not completed, move to next block
      int64_t start = taosGetTimestampUs();

      blockLoadStatus = moveToNextBlock(pRuntimeEnv, step, searchFn, LOAD_DATA);
      if (Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK | QUERY_COMPLETED)) {
        savePointPosition(&pRuntimeEnv->nextPos, pQuery->fileId, pQuery->slot, pQuery->pos);
        setQueryStatus(pQuery, QUERY_COMPLETED);
        break;
      }

      int64_t delta = (taosGetTimestampUs() - start);
      if (IS_DISK_DATA_BLOCK(pQuery)) {
        pSummary->fileTimeUs += delta;
      } else {
        pSummary->cacheTimeUs += delta;
      }
    }

    // check next block
    void *pNextBlock = getGenericDataBlock(pMeterObj, pRuntimeEnv, pQuery->slot);

    int32_t blockType = (IS_DISK_DATA_BLOCK(pQuery)) ? BLK_FILE_BLOCK : BLK_CACHE_BLOCK;
    blockInfo = getBlockBasicInfo(pRuntimeEnv, pNextBlock, blockType);
    if (!checkQueryRangeAgainstNextBlock(&blockInfo, pRuntimeEnv)) {
      break;
    }
  }  // while(1)

  return cnt;
}

static void updatelastkey(SQuery *pQuery, SMeterQueryInfo *pMeterQInfo) { pMeterQInfo->lastKey = pQuery->lastKey; }

void queryOnBlock(SMeterQuerySupportObj *pSupporter, int64_t *primaryKeys, int32_t blockStatus,
                  SBlockInfo *pBlockBasicInfo, SMeterDataInfo *pMeterDataInfo, SField *pFields,
                  __block_search_fn_t searchFn) {
  /* cache blocks may be assign to other meter, abort */
  if (pBlockBasicInfo->size <= 0) {
    return;
  }

  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  if (pQuery->nAggTimeInterval == 0) {  // not interval query
    int32_t numOfRes = 0;
    applyFunctionsOnBlock(pRuntimeEnv, pBlockBasicInfo, primaryKeys, pFields, searchFn, &numOfRes);

    // note: only fixed number of output for each group by operation
    if (numOfRes > 0) {
      pSupporter->pResult[pMeterDataInfo->groupIdx].numOfRows = numOfRes;
    }

    // used to decide the correct start position in cache after check all data in files
    updatelastkey(pQuery, pMeterDataInfo->pMeterQInfo);
    if (pRuntimeEnv->pTSBuf != NULL) {
      pMeterDataInfo->pMeterQInfo->cur = tsBufGetCursor(pRuntimeEnv->pTSBuf);
    }

  } else {
    applyIntervalQueryOnBlock(pSupporter, pMeterDataInfo, pBlockBasicInfo, blockStatus, pFields, searchFn);
  }
}

/*
 * set tag value in SQLFunctionCtx
 * e.g.,tag information into input buffer
 */
static void doSetTagValueInParam(tTagSchema *pTagSchema, int32_t tagColIdx, SMeterSidExtInfo *pMeterSidInfo,
                                 tVariant *param) {
  assert(tagColIdx >= 0);

  int32_t *fieldValueOffset = pTagSchema->colOffset;

  void *   pStr = (char *)pMeterSidInfo->tags + fieldValueOffset[tagColIdx];
  SSchema *pCol = &pTagSchema->pSchema[tagColIdx];

  tVariantDestroy(param);

  if (isNull(pStr, pCol->type)) {
    param->nType = TSDB_DATA_TYPE_NULL;
  } else {
    tVariantCreateFromBinary(param, pStr, pCol->bytes, pCol->type);
  }
}

void vnodeSetTagValueInParam(tSidSet *pSidSet, SQueryRuntimeEnv *pRuntimeEnv, SMeterSidExtInfo *pMeterSidInfo) {
  SQuery *    pQuery = pRuntimeEnv->pQuery;
  tTagSchema *pTagSchema = pSidSet->pTagSchema;

  SSqlFuncExprMsg *pFuncMsg = &pQuery->pSelectExpr[0].pBase;
  if (pQuery->numOfOutputCols == 1 && pFuncMsg->functionId == TSDB_FUNC_TS_COMP) {
    assert(pFuncMsg->numOfParams == 1);
    doSetTagValueInParam(pTagSchema, pFuncMsg->arg->argValue.i64, pMeterSidInfo, &pRuntimeEnv->pCtx[0].tag);
  } else {
    // set tag value, by which the results are aggregated.
    for (int32_t idx = 0; idx < pQuery->numOfOutputCols; ++idx) {
      SColIndexEx *pColEx = &pQuery->pSelectExpr[idx].pBase.colInfo;

      // ts_comp column required the tag value for join filter
      if (!TSDB_COL_IS_TAG(pColEx->flag)) {
        continue;
      }

      doSetTagValueInParam(pTagSchema, pColEx->colIdx, pMeterSidInfo, &pRuntimeEnv->pCtx[idx].tag);
    }

    // set the join tag for first column
    SSqlFuncExprMsg *pFuncMsg = &pQuery->pSelectExpr[0].pBase;
    if (pFuncMsg->functionId == TSDB_FUNC_TS && pFuncMsg->colInfo.colIdx == PRIMARYKEY_TIMESTAMP_COL_INDEX &&
        pRuntimeEnv->pTSBuf != NULL) {
      assert(pFuncMsg->numOfParams == 1);
      doSetTagValueInParam(pTagSchema, pFuncMsg->arg->argValue.i64, pMeterSidInfo, &pRuntimeEnv->pCtx[0].tag);
    }
  }
}

static void doMerge(SQueryRuntimeEnv *pRuntimeEnv, int64_t timestamp, tFilePage *inputSrc, int32_t inputIdx,
                    bool mergeFlag) {
  SQuery *        pQuery = pRuntimeEnv->pQuery;
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (!mergeFlag) {
      pCtx[i].aOutputBuf = pCtx[i].aOutputBuf + pCtx[i].outputBytes;
      pCtx[i].currentStage = FIRST_STAGE_MERGE;

      resetResultInfo(pCtx[i].resultInfo);
      aAggs[functionId].init(&pCtx[i]);
    }

    pCtx[i].hasNull = true;
    pCtx[i].nStartQueryTimestamp = timestamp;
    pCtx[i].aInputElemBuf = ((char *)inputSrc->data) +
                            ((int32_t)pRuntimeEnv->offset[i] * pRuntimeEnv->numOfRowsPerPage) +
                            pCtx[i].outputBytes * inputIdx;

    // in case of tag column, the tag information should be extracted from input buffer
    if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TAG) {
      tVariantDestroy(&pCtx[i].tag);
      tVariantCreateFromBinary(&pCtx[i].tag, pCtx[i].aInputElemBuf, pCtx[i].inputBytes, pCtx[i].inputType);
    }
  }

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TAG_DUMMY) {
      continue;
    }

    aAggs[functionId].distMergeFunc(&pCtx[i]);
  }
}

static void printBinaryData(int32_t functionId, char *data, int32_t srcDataType) {
  if (functionId == TSDB_FUNC_FIRST_DST || functionId == TSDB_FUNC_LAST_DST) {
    switch (srcDataType) {
      case TSDB_DATA_TYPE_BINARY:
        printf("%" PRId64 ",%s\t", *(TSKEY *)data, (data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_TINYINT:
      case TSDB_DATA_TYPE_BOOL:
        printf("%" PRId64 ",%d\t", *(TSKEY *)data, *(int8_t *)(data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_SMALLINT:
        printf("%" PRId64 ",%d\t", *(TSKEY *)data, *(int16_t *)(data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_BIGINT:
      case TSDB_DATA_TYPE_TIMESTAMP:
        printf("%" PRId64 ",%" PRId64 "\t", *(TSKEY *)data, *(TSKEY *)(data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_INT:
        printf("%" PRId64 ",%d\t", *(TSKEY *)data, *(int32_t *)(data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_FLOAT:
        printf("%" PRId64 ",%f\t", *(TSKEY *)data, *(float *)(data + TSDB_KEYSIZE + 1));
        break;
      case TSDB_DATA_TYPE_DOUBLE:
        printf("%" PRId64 ",%lf\t", *(TSKEY *)data, *(double *)(data + TSDB_KEYSIZE + 1));
        break;
    }
  } else if (functionId == TSDB_FUNC_AVG) {
    printf("%lf,%d\t", *(double *)data, *(int32_t *)(data + sizeof(double)));
  } else if (functionId == TSDB_FUNC_SPREAD) {
    printf("%lf,%lf\t", *(double *)data, *(double *)(data + sizeof(double)));
  } else if (functionId == TSDB_FUNC_TWA) {
    data += 1;
    printf("%lf,%" PRId64 ",%" PRId64 ",%" PRId64 "\t", *(double *)data, *(int64_t *)(data + 8),
           *(int64_t *)(data + 16), *(int64_t *)(data + 24));
  } else if (functionId == TSDB_FUNC_MIN || functionId == TSDB_FUNC_MAX) {
    switch (srcDataType) {
      case TSDB_DATA_TYPE_TINYINT:
      case TSDB_DATA_TYPE_BOOL:
        printf("%d\t", *(int8_t *)data);
        break;
      case TSDB_DATA_TYPE_SMALLINT:
        printf("%d\t", *(int16_t *)data);
        break;
      case TSDB_DATA_TYPE_BIGINT:
      case TSDB_DATA_TYPE_TIMESTAMP:
        printf("%" PRId64 "\t", *(int64_t *)data);
        break;
      case TSDB_DATA_TYPE_INT:
        printf("%d\t", *(int *)data);
        break;
      case TSDB_DATA_TYPE_FLOAT:
        printf("%f\t", *(float *)data);
        break;
      case TSDB_DATA_TYPE_DOUBLE:
        printf("%f\t", *(float *)data);
        break;
    }
  } else if (functionId == TSDB_FUNC_SUM) {
    if (srcDataType == TSDB_DATA_TYPE_FLOAT || srcDataType == TSDB_DATA_TYPE_DOUBLE) {
      printf("%lf\t", *(float *)data);
    } else {
      printf("%" PRId64 "\t", *(int64_t *)data);
    }
  } else {
    printf("%s\t", data);
  }
}

void UNUSED_FUNC displayInterResult(SData **pdata, SQuery *pQuery, int32_t numOfRows) {
  int32_t numOfCols = pQuery->numOfOutputCols;
  printf("metric query intern-result, total:%d\n", numOfRows);

  SQInfo *   pQInfo = (SQInfo *)(GET_QINFO_ADDR(pQuery));
  SMeterObj *pMeterObj = pQInfo->pObj;

  for (int32_t j = 0; j < numOfRows; ++j) {
    for (int32_t i = 0; i < numOfCols; ++i) {
      switch (pQuery->pSelectExpr[i].resType) {
        case TSDB_DATA_TYPE_BINARY: {
          int32_t colIdx = pQuery->pSelectExpr[i].pBase.colInfo.colIdx;
          int32_t type = 0;

          if (TSDB_COL_IS_TAG(pQuery->pSelectExpr[i].pBase.colInfo.flag)) {
            type = pQuery->pSelectExpr[i].resType;
          } else {
            type = pMeterObj->schema[colIdx].type;
          }
          printBinaryData(pQuery->pSelectExpr[i].pBase.functionId, pdata[i]->data + pQuery->pSelectExpr[i].resBytes * j,
                          type);
          break;
        }
        case TSDB_DATA_TYPE_TIMESTAMP:
        case TSDB_DATA_TYPE_BIGINT:
          printf("%" PRId64 "\t", *(int64_t *)(pdata[i]->data + pQuery->pSelectExpr[i].resBytes * j));
          break;
        case TSDB_DATA_TYPE_INT:
          printf("%d\t", *(int32_t *)(pdata[i]->data + pQuery->pSelectExpr[i].resBytes * j));
          break;
        case TSDB_DATA_TYPE_FLOAT:
          printf("%f\t", *(float *)(pdata[i]->data + pQuery->pSelectExpr[i].resBytes * j));
          break;
        case TSDB_DATA_TYPE_DOUBLE:
          printf("%lf\t", *(double *)(pdata[i]->data + pQuery->pSelectExpr[i].resBytes * j));
          break;
      }
    }
    printf("\n");
  }
}

static tFilePage *getFilePage(SMeterQuerySupportObj *pSupporter, int32_t pageId) {
  assert(pageId <= pSupporter->lastPageId && pageId >= 0);
  return (tFilePage *)(pSupporter->meterOutputMMapBuf + DEFAULT_INTERN_BUF_SIZE * pageId);
}

static tFilePage *getMeterDataPage(SMeterQuerySupportObj *pSupporter, SMeterDataInfo *pMeterDataInfo, int32_t pageId) {
  SMeterQueryInfo *pMeterQueryInfo = pMeterDataInfo->pMeterQInfo;
  if (pageId >= pMeterQueryInfo->numOfPages) {
    return NULL;
  }

  int32_t realId = pMeterQueryInfo->pageList[pageId];
  return getFilePage(pSupporter, realId);
}

typedef struct Position {
  int32_t pageIdx;
  int32_t rowIdx;
} Position;

typedef struct SCompSupporter {
  SMeterDataInfo **      pMeterDataInfo;
  Position *             pPosition;
  SMeterQuerySupportObj *pSupporter;
} SCompSupporter;

int64_t getCurrentTimestamp(SCompSupporter *pSupportor, int32_t meterIdx) {
  Position * pPos = &pSupportor->pPosition[meterIdx];
  tFilePage *pPage = getMeterDataPage(pSupportor->pSupporter, pSupportor->pMeterDataInfo[meterIdx], pPos->pageIdx);
  return *(int64_t *)(pPage->data + TSDB_KEYSIZE * pPos->rowIdx);
}

int32_t meterResultComparator(const void *pLeft, const void *pRight, void *param) {
  int32_t left = *(int32_t *)pLeft;
  int32_t right = *(int32_t *)pRight;

  SCompSupporter *supportor = (SCompSupporter *)param;

  Position leftPos = supportor->pPosition[left];
  Position rightPos = supportor->pPosition[right];

  /* left source is exhausted */
  if (leftPos.pageIdx == -1 && leftPos.rowIdx == -1) {
    return 1;
  }

  /* right source is exhausted*/
  if (rightPos.pageIdx == -1 && rightPos.rowIdx == -1) {
    return -1;
  }

  tFilePage *pPageLeft = getMeterDataPage(supportor->pSupporter, supportor->pMeterDataInfo[left], leftPos.pageIdx);
  int64_t    leftTimestamp = *(int64_t *)(pPageLeft->data + TSDB_KEYSIZE * leftPos.rowIdx);

  tFilePage *pPageRight = getMeterDataPage(supportor->pSupporter, supportor->pMeterDataInfo[right], rightPos.pageIdx);
  int64_t    rightTimestamp = *(int64_t *)(pPageRight->data + TSDB_KEYSIZE * rightPos.rowIdx);

  if (leftTimestamp == rightTimestamp) {
    return 0;
  }

  return leftTimestamp > rightTimestamp ? 1 : -1;
}

int32_t mergeMetersResultToOneGroups(SMeterQuerySupportObj *pSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  int64_t st = taosGetTimestampMs();
  int32_t ret = TSDB_CODE_SUCCESS;

  while (pSupporter->subgroupIdx < pSupporter->pSidSet->numOfSubSet) {
    int32_t start = pSupporter->pSidSet->starterPos[pSupporter->subgroupIdx];
    int32_t end = pSupporter->pSidSet->starterPos[pSupporter->subgroupIdx + 1];

    ret = doMergeMetersResultsToGroupRes(pSupporter, pQuery, pRuntimeEnv, pSupporter->pMeterDataInfo, start, end);
    if (ret < 0) {  // not enough disk space to save the data into disk
      return -1;
    }

    pSupporter->subgroupIdx += 1;

    // this group generates at least one result, return results
    if (ret > 0) {
      break;
    }

    assert(pSupporter->numOfGroupResultPages == 0);
    dTrace("QInfo:%p no result in group %d, continue", GET_QINFO_ADDR(pQuery), pSupporter->subgroupIdx - 1);
  }

  dTrace("QInfo:%p merge res data into group, index:%d, total group:%d, elapsed time:%lldms", GET_QINFO_ADDR(pQuery),
         pSupporter->subgroupIdx - 1, pSupporter->pSidSet->numOfSubSet, taosGetTimestampMs() - st);

  return TSDB_CODE_SUCCESS;
}

void copyResToQueryResultBuf(SMeterQuerySupportObj *pSupporter, SQuery *pQuery) {
  if (pSupporter->offset == pSupporter->numOfGroupResultPages) {
    pSupporter->numOfGroupResultPages = 0;

    // current results of group has been sent to client, try next group
    if (mergeMetersResultToOneGroups(pSupporter) != TSDB_CODE_SUCCESS) {
      return;  // failed to save data in the disk
    }

    // set current query completed
    if (pSupporter->numOfGroupResultPages == 0 && pSupporter->subgroupIdx == pSupporter->pSidSet->numOfSubSet) {
      pSupporter->meterIdx = pSupporter->pSidSet->numOfSids;
      return;
    }
  }

  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  char *            pStart = pSupporter->meterOutputMMapBuf + DEFAULT_INTERN_BUF_SIZE * (pSupporter->lastPageId + 1) +
                 pSupporter->groupResultSize * pSupporter->offset;

  uint64_t numOfElem = ((tFilePage *)pStart)->numOfElems;
  assert(numOfElem <= pQuery->pointsToRead);

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    memcpy(pQuery->sdata[i], pStart, pRuntimeEnv->pCtx[i].outputBytes * numOfElem + sizeof(tFilePage));
    pStart += pRuntimeEnv->pCtx[i].outputBytes * pQuery->pointsToRead + sizeof(tFilePage);
  }

  pQuery->pointsRead += numOfElem;
  pSupporter->offset += 1;
}

int32_t doMergeMetersResultsToGroupRes(SMeterQuerySupportObj *pSupporter, SQuery *pQuery, SQueryRuntimeEnv *pRuntimeEnv,
                                       SMeterDataInfo *pMeterHeadDataInfo, int32_t start, int32_t end) {
  // calculate the maximum required space
  if (pSupporter->groupResultSize == 0) {
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      pSupporter->groupResultSize += sizeof(tFilePage) + pQuery->pointsToRead * pRuntimeEnv->pCtx[i].outputBytes;
    }
  }

  tFilePage **     buffer = (tFilePage **)pQuery->sdata;
  Position *       posArray = calloc(1, sizeof(Position) * (end - start));
  SMeterDataInfo **pValidMeter = malloc(POINTER_BYTES * (end - start));

  int32_t numOfMeters = 0;
  for (int32_t i = start; i < end; ++i) {
    if (pMeterHeadDataInfo[i].pMeterQInfo->numOfPages > 0 && pMeterHeadDataInfo[i].pMeterQInfo->numOfRes > 0) {
      pValidMeter[numOfMeters] = &pMeterHeadDataInfo[i];
      // set the merge start position: page:0, index:0
      posArray[numOfMeters].pageIdx = 0;
      posArray[numOfMeters++].rowIdx = 0;
    }
  }

  if (numOfMeters == 0) {
    tfree(posArray);
    tfree(pValidMeter);
    assert(pSupporter->numOfGroupResultPages == 0);
    return 0;
  }

  SCompSupporter  cs = {pValidMeter, posArray, pSupporter};
  SLoserTreeInfo *pTree = NULL;

  tLoserTreeCreate(&pTree, numOfMeters, &cs, meterResultComparator);

  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;
  resetMergeResultBuf(pQuery, pCtx);

  int64_t lastTimestamp = -1;

  int64_t startt = taosGetTimestampMs();

  while (1) {
    int32_t    pos = pTree->pNode[0].index;
    Position * position = &cs.pPosition[pos];
    tFilePage *pPage = getMeterDataPage(cs.pSupporter, pValidMeter[pos], position->pageIdx);

    int64_t ts = getCurrentTimestamp(&cs, pos);
    if (ts == lastTimestamp) {  // merge with the last one
      doMerge(pRuntimeEnv, ts, pPage, position->rowIdx, true);
    } else {
      // copy data to disk buffer
      if (buffer[0]->numOfElems == pQuery->pointsToRead) {
        if (flushFromResultBuf(pSupporter, pQuery, pRuntimeEnv) != TSDB_CODE_SUCCESS) {
          return -1;
        }

        resetMergeResultBuf(pQuery, pCtx);
      }

      pPage = getMeterDataPage(cs.pSupporter, pValidMeter[pos], position->pageIdx);
      if (pPage->numOfElems <= 0) {  // current source data page is empty
        // do nothing
      } else {
        doMerge(pRuntimeEnv, ts, pPage, position->rowIdx, false);
        buffer[0]->numOfElems += 1;
      }
    }

    lastTimestamp = ts;

    if (cs.pPosition[pos].rowIdx >= pPage->numOfElems - 1) {
      cs.pPosition[pos].rowIdx = 0;
      cs.pPosition[pos].pageIdx += 1;  // try next page

      // check if current page is empty or not. if it is empty, ignore it and try next
      if (cs.pPosition[pos].pageIdx <= cs.pMeterDataInfo[pos]->pMeterQInfo->numOfPages - 1) {
        tFilePage *newPage = getMeterDataPage(cs.pSupporter, pValidMeter[pos], position->pageIdx);
        if (newPage->numOfElems <= 0) {
          // if current source data page is null, it must be the last page of source output page
          cs.pPosition[pos].pageIdx += 1;
          assert(cs.pPosition[pos].pageIdx >= cs.pMeterDataInfo[pos]->pMeterQInfo->numOfPages - 1);
        }
      }

      // the following code must be executed if current source pages are exhausted
      if (cs.pPosition[pos].pageIdx >= cs.pMeterDataInfo[pos]->pMeterQInfo->numOfPages) {
        cs.pPosition[pos].pageIdx = -1;
        cs.pPosition[pos].rowIdx = -1;

        // all input sources are exhausted
        if (--numOfMeters == 0) {
          break;
        }
      }
    } else {
      cs.pPosition[pos].rowIdx += 1;
    }

    tLoserTreeAdjust(pTree, pos + pTree->numOfEntries);
  }

  if (buffer[0]->numOfElems != 0) {  // there are data in buffer
    if (flushFromResultBuf(pSupporter, pQuery, pRuntimeEnv) != TSDB_CODE_SUCCESS) {
      dError("QInfo:%p failed to flush data into temp file, abort query", GET_QINFO_ADDR(pQuery),
             pSupporter->extBufFile);
      tfree(pTree);
      tfree(pValidMeter);
      tfree(posArray);

      return -1;
    }
  }

  int64_t endt = taosGetTimestampMs();

#ifdef _DEBUG_VIEW
  displayInterResult(pQuery->sdata, pQuery, pQuery->sdata[0]->len);
#endif

  dTrace("QInfo:%p result merge completed, elapsed time:%" PRId64 " ms", GET_QINFO_ADDR(pQuery), endt - startt);
  tfree(pTree);
  tfree(pValidMeter);
  tfree(posArray);

  pSupporter->offset = 0;

  return pSupporter->numOfGroupResultPages;
}

static int32_t extendDiskBuf(const SQuery *pQuery, SMeterQuerySupportObj *pSupporter, int32_t numOfPages) {
  assert(pSupporter->numOfPages * DEFAULT_INTERN_BUF_SIZE == pSupporter->bufSize);

  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  int32_t ret = munmap(pSupporter->meterOutputMMapBuf, pSupporter->bufSize);
  pSupporter->numOfPages = numOfPages;

  /*
   * disk-based output buffer is exhausted, try to extend the disk-based buffer, the available disk space may
   * be insufficient
   */
  ret = ftruncate(pSupporter->meterOutputFd, pSupporter->numOfPages * DEFAULT_INTERN_BUF_SIZE);
  if (ret != 0) {
    dError("QInfo:%p failed to create intermediate result output file:%s. %s", pQInfo, pSupporter->extBufFile,
           strerror(errno));
    pQInfo->code = -TSDB_CODE_SERV_NO_DISKSPACE;
    pQInfo->killed = 1;

    return pQInfo->code;
  }

  pSupporter->bufSize = pSupporter->numOfPages * DEFAULT_INTERN_BUF_SIZE;
  pSupporter->meterOutputMMapBuf =
      mmap(NULL, pSupporter->bufSize, PROT_READ | PROT_WRITE, MAP_SHARED, pSupporter->meterOutputFd, 0);

  if (pSupporter->meterOutputMMapBuf == MAP_FAILED) {
    dError("QInfo:%p failed to map temp file: %s. %s", pQInfo, pSupporter->extBufFile, strerror(errno));
    pQInfo->code = -TSDB_CODE_SERV_OUT_OF_MEMORY;
    pQInfo->killed = 1;

    return pQInfo->code;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t flushFromResultBuf(SMeterQuerySupportObj *pSupporter, const SQuery *pQuery,
                           const SQueryRuntimeEnv *pRuntimeEnv) {
  int32_t numOfMeterResultBufPages = pSupporter->lastPageId + 1;
  int64_t dstSize = numOfMeterResultBufPages * DEFAULT_INTERN_BUF_SIZE +
                    pSupporter->groupResultSize * (pSupporter->numOfGroupResultPages + 1);

  int32_t requiredPages = pSupporter->numOfPages;
  if (requiredPages * DEFAULT_INTERN_BUF_SIZE < dstSize) {
    while (requiredPages * DEFAULT_INTERN_BUF_SIZE < dstSize) {
      requiredPages += pSupporter->numOfMeters;
    }

    if (extendDiskBuf(pQuery, pSupporter, requiredPages) != TSDB_CODE_SUCCESS) {
      return -1;
    }
  }

  char *lastPosition = pSupporter->meterOutputMMapBuf + DEFAULT_INTERN_BUF_SIZE * numOfMeterResultBufPages +
                       pSupporter->groupResultSize * pSupporter->numOfGroupResultPages;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t size = pRuntimeEnv->pCtx[i].outputBytes * pQuery->sdata[0]->len + sizeof(tFilePage);
    memcpy(lastPosition, pQuery->sdata[i], size);

    lastPosition += pRuntimeEnv->pCtx[i].outputBytes * pQuery->pointsToRead + sizeof(tFilePage);
  }

  pSupporter->numOfGroupResultPages += 1;
  return TSDB_CODE_SUCCESS;
}

void resetMergeResultBuf(SQuery *pQuery, SQLFunctionCtx *pCtx) {
  for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
    pCtx[k].aOutputBuf = pQuery->sdata[k]->data - pCtx[k].outputBytes;
    pCtx[k].size = 1;
    pCtx[k].startOffset = 0;
    pQuery->sdata[k]->len = 0;
  }
}

void setMeterDataInfo(SMeterDataInfo *pMeterDataInfo, SMeterObj *pMeterObj, int32_t meterIdx, int32_t groupId) {
  pMeterDataInfo->pMeterObj = pMeterObj;
  pMeterDataInfo->groupIdx = groupId;
  pMeterDataInfo->meterOrderIdx = meterIdx;
}

int32_t doCloseAllOpenedResults(SMeterQuerySupportObj *pSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  /* for interval query, close all unclosed results */
  if (pQuery->nAggTimeInterval > 0) {
    SMeterDataInfo *pMeterInfo = pSupporter->pMeterDataInfo;
    for (int32_t i = 0; i < pSupporter->numOfMeters; ++i) {
      if (pMeterInfo[i].pMeterQInfo != NULL && pMeterInfo[i].pMeterQInfo->lastResRows > 0) {
        int32_t index = pMeterInfo[i].meterOrderIdx;

        pRuntimeEnv->pMeterObj = getMeterObj(pSupporter->pMetersHashTable, pSupporter->pSidSet->pSids[index]->sid);
        assert(pRuntimeEnv->pMeterObj == pMeterInfo[i].pMeterObj);

        int32_t ret = setIntervalQueryExecutionContext(pSupporter, i, pMeterInfo[i].pMeterQInfo);
        if (ret != TSDB_CODE_SUCCESS) {
          return ret;
        }

        ret = saveResult(pSupporter, pMeterInfo[i].pMeterQInfo, pMeterInfo[i].pMeterQInfo->lastResRows);
        if (ret != TSDB_CODE_SUCCESS) {
          return ret;
        }
      }
    }
  }

  return TSDB_CODE_SUCCESS;
}

void disableFunctForSuppleScan(SQueryRuntimeEnv *pRuntimeEnv, int32_t order) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->slidingTime > 0 && pQuery->nAggTimeInterval > 0)) {
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      pRuntimeEnv->pCtx[i].order = (pRuntimeEnv->pCtx[i].order) ^ 1;
    }

    SSlidingWindowInfo *pSlidingWindowInfo = &pRuntimeEnv->swindowResInfo;

    for (int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
      SWindowStatus *pStatus = &pSlidingWindowInfo->pStatus[i];
      if (!pStatus->closed) {
        continue;
      }

      SOutputRes *buf = &pSlidingWindowInfo->pResult[i];

      // open/close the specified query for each group result
      for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
        int32_t functId = pQuery->pSelectExpr[j].pBase.functionId;

        if (((functId == TSDB_FUNC_FIRST || functId == TSDB_FUNC_FIRST_DST) && order == TSQL_SO_DESC) ||
            ((functId == TSDB_FUNC_LAST || functId == TSDB_FUNC_LAST_DST) && order == TSQL_SO_ASC)) {
          buf->resultInfo[j].complete = false;
        } else if (functId != TSDB_FUNC_TS && functId != TSDB_FUNC_TAG) {
          buf->resultInfo[j].complete = true;
        }
      }
    }
  } else {  // TODO ERROR!!
    // need to handle for each query result, not just the single runtime ctx.
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      pRuntimeEnv->pCtx[i].order = (pRuntimeEnv->pCtx[i].order) ^ 1;
      int32_t functId = pQuery->pSelectExpr[i].pBase.functionId;

      SResultInfo *pResInfo = GET_RES_INFO(&pRuntimeEnv->pCtx[i]);
      if (((functId == TSDB_FUNC_FIRST || functId == TSDB_FUNC_FIRST_DST) && order == TSQL_SO_DESC) ||
          ((functId == TSDB_FUNC_LAST || functId == TSDB_FUNC_LAST_DST) && order == TSQL_SO_ASC)) {
        pResInfo->complete = false;

      } else if (functId != TSDB_FUNC_TS && functId != TSDB_FUNC_TAG) {
        pResInfo->complete = true;
      }
    }
  }

  pQuery->order.order = pQuery->order.order ^ 1;
}

void enableFunctForMasterScan(SQueryRuntimeEnv *pRuntimeEnv, int32_t order) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    pRuntimeEnv->pCtx[i].order = (pRuntimeEnv->pCtx[i].order) ^ 1;
  }

  pQuery->order.order = (pQuery->order.order ^ 1);
}

void createGroupResultBuf(SQuery *pQuery, SOutputRes *pOneResult, bool isMetricQuery) {
  int32_t numOfOutput = pQuery->numOfOutputCols;

  pOneResult->resultInfo = calloc((size_t)numOfOutput, sizeof(SResultInfo));

  pOneResult->result = malloc(POINTER_BYTES * numOfOutput);
  for (int32_t i = 0; i < numOfOutput; ++i) {
    size_t       size = pQuery->pSelectExpr[i].interResBytes;
    SResultInfo *pResInfo = &pOneResult->resultInfo[i];

    pOneResult->result[i] = malloc(sizeof(tFilePage) + size * pOneResult->nAlloc);
    pOneResult->result[i]->numOfElems = 0;

    setResultInfoBuf(pResInfo, (int32_t)size, isMetricQuery);
  }
}

void clearGroupResultBuf(SOutputRes *pOneOutputRes, int32_t nOutputCols) {
  if (pOneOutputRes == NULL) {
    return;
  }

  for (int32_t i = 0; i < nOutputCols; ++i) {
    SResultInfo *pResInfo = &pOneOutputRes->resultInfo[i];
    int32_t      size = sizeof(tFilePage) + pResInfo->bufLen * pOneOutputRes->nAlloc;

    memset(pOneOutputRes->result[i], 0, (size_t)size);
    resetResultInfo(pResInfo);
  }
}

void copyGroupResultBuf(SOutputRes* dst, const SOutputRes* src, int32_t nOutputCols) {
  for(int32_t i = 0; i < nOutputCols; ++i) {
    SResultInfo *pDst = &dst->resultInfo[i];
    SResultInfo *pSrc = &src->resultInfo[i];
    
    char* buf = pDst->interResultBuf;
    memcpy(pDst, pSrc, sizeof(SResultInfo));
    pDst->interResultBuf = buf;
    memcpy(pDst->interResultBuf, pSrc->interResultBuf, pDst->bufLen);
  
    int32_t size = sizeof(tFilePage) + pSrc->bufLen * src->nAlloc;
    memcpy(dst->result[i], src->result[i], size);
  }
}

void destroyGroupResultBuf(SOutputRes *pOneOutputRes, int32_t nOutputCols) {
  if (pOneOutputRes == NULL) {
    return;
  }

  for (int32_t i = 0; i < nOutputCols; ++i) {
    free(pOneOutputRes->result[i]);
    free(pOneOutputRes->resultInfo[i].interResultBuf);
  }

  free(pOneOutputRes->resultInfo);
  free(pOneOutputRes->result);
}

void resetCtxOutputBuf(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  int32_t rows = pRuntimeEnv->pMeterObj->pointsPerFileBlock;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];
    pCtx->aOutputBuf = pQuery->sdata[i]->data;

    /*
     * set the output buffer information and intermediate buffer
     * not all queries require the interResultBuf, such as COUNT/TAGPRJ/PRJ/TAG etc.
     */
    resetResultInfo(&pRuntimeEnv->resultInfo[i]);
    pCtx->resultInfo = &pRuntimeEnv->resultInfo[i];

    // set the timestamp output buffer for top/bottom/diff query
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM || functionId == TSDB_FUNC_DIFF) {
      pCtx->ptsOutputBuf = pRuntimeEnv->pCtx[0].aOutputBuf;
    }

    memset(pQuery->sdata[i]->data, 0, (size_t)pQuery->pSelectExpr[i].resBytes * rows);
  }

  initCtxOutputBuf(pRuntimeEnv);
}

void forwardCtxOutputBuf(SQueryRuntimeEnv *pRuntimeEnv, int64_t output) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  // reset the execution contexts
  for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
    int32_t functionId = pQuery->pSelectExpr[j].pBase.functionId;
    assert(functionId != TSDB_FUNC_DIFF);

    // set next output position
    if (IS_OUTER_FORWARD(aAggs[functionId].nStatus)) {
      pRuntimeEnv->pCtx[j].aOutputBuf += pRuntimeEnv->pCtx[j].outputBytes * output /** factor*/;
    }

    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM) {
      /*
       * NOTE: for top/bottom query, the value of first column of output (timestamp) are assigned
       * in the procedure of top/bottom routine
       * the output buffer in top/bottom routine is ptsOutputBuf, so we need to forward the output buffer
       *
       * diff function is handled in multi-output function
       */
      pRuntimeEnv->pCtx[j].ptsOutputBuf += TSDB_KEYSIZE * output/* * factor*/;
    }

    resetResultInfo(pRuntimeEnv->pCtx[j].resultInfo);
  }
}

void initCtxOutputBuf(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
    int32_t functionId = pQuery->pSelectExpr[j].pBase.functionId;
    pRuntimeEnv->pCtx[j].currentStage = 0;
    aAggs[functionId].init(&pRuntimeEnv->pCtx[j]);
  }
}

void doSkipResults(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  if (pQuery->pointsRead == 0 || pQuery->limit.offset == 0) {
    return;
  }

  if (pQuery->pointsRead <= pQuery->limit.offset) {
    pQuery->limit.offset -= pQuery->pointsRead;

    pQuery->pointsRead = 0;
    pQuery->pointsOffset = pQuery->pointsToRead;  // clear all data in result buffer

    resetCtxOutputBuf(pRuntimeEnv);

    // clear the buffer is full flag if exists
    pQuery->over &= (~QUERY_RESBUF_FULL);
  } else {
    int32_t numOfSkip = (int32_t)pQuery->limit.offset;
    pQuery->pointsRead -= numOfSkip;

    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
      int32_t bytes = pRuntimeEnv->pCtx[i].outputBytes;

      memmove(pQuery->sdata[i]->data, pQuery->sdata[i]->data + bytes * numOfSkip, pQuery->pointsRead * bytes);
      pRuntimeEnv->pCtx[i].aOutputBuf += bytes * numOfSkip;

      if (functionId == TSDB_FUNC_DIFF || functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM) {
        pRuntimeEnv->pCtx[i].ptsOutputBuf += TSDB_KEYSIZE * numOfSkip;
      }
    }

    pQuery->limit.offset = 0;
  }
}

typedef struct SQueryStatus {
  SPositionInfo start;
  SPositionInfo next;
  SPositionInfo end;

  TSKEY  skey;
  TSKEY  ekey;
  int8_t overStatus;
  TSKEY  lastKey;

  STSCursor cur;
} SQueryStatus;

static void queryStatusSave(SQueryRuntimeEnv *pRuntimeEnv, SQueryStatus *pStatus) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  pStatus->overStatus = pQuery->over;
  pStatus->lastKey = pQuery->lastKey;

  pStatus->skey = pQuery->skey;
  pStatus->ekey = pQuery->ekey;

  pStatus->start = pRuntimeEnv->startPos;
  pStatus->next = pRuntimeEnv->nextPos;
  pStatus->end = pRuntimeEnv->endPos;

  pStatus->cur = tsBufGetCursor(pRuntimeEnv->pTSBuf);  // save the cursor

  if (pRuntimeEnv->pTSBuf) {
    pRuntimeEnv->pTSBuf->cur.order ^= 1;
    tsBufNextPos(pRuntimeEnv->pTSBuf);
  }

  setQueryStatus(pQuery, QUERY_NOT_COMPLETED);

  SWAP(pQuery->skey, pQuery->ekey, TSKEY);
  pQuery->lastKey = pQuery->skey;
  pRuntimeEnv->startPos = pRuntimeEnv->endPos;
  
  SWAP(pRuntimeEnv->intervalWindow.skey, pRuntimeEnv->intervalWindow.ekey, TSKEY);
}

static void queryStatusRestore(SQueryRuntimeEnv *pRuntimeEnv, SQueryStatus *pStatus) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  SWAP(pQuery->skey, pQuery->ekey, TSKEY);
  SWAP(pRuntimeEnv->intervalWindow.skey, pRuntimeEnv->intervalWindow.ekey, TSKEY);
  
  pQuery->lastKey = pStatus->lastKey;
  pQuery->skey = pStatus->skey;
  pQuery->ekey = pStatus->ekey;

  pQuery->over = pStatus->overStatus;

  pRuntimeEnv->startPos = pStatus->start;
  pRuntimeEnv->nextPos = pStatus->next;
  pRuntimeEnv->endPos = pStatus->end;

  tsBufSetCursor(pRuntimeEnv->pTSBuf, &pStatus->cur);
}

static void doSingleMeterSupplementScan(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *     pQuery = pRuntimeEnv->pQuery;
  SQueryStatus qStatus = {0};

  if (!needSupplementaryScan(pQuery)) {
    return;
  }

  dTrace("QInfo:%p start to supp scan", GET_QINFO_ADDR(pQuery));
  
  SET_SUPPLEMENT_SCAN_FLAG(pRuntimeEnv);

  // usually this load operation will incur load disk block operation
  TSKEY endKey = loadRequiredBlockIntoMem(pRuntimeEnv, &pRuntimeEnv->endPos);

  assert((QUERY_IS_ASC_QUERY(pQuery) && endKey <= pQuery->ekey) ||
         (!QUERY_IS_ASC_QUERY(pQuery) && endKey >= pQuery->ekey));

  // close necessary function execution during supplementary scan
  disableFunctForSuppleScan(pRuntimeEnv, pQuery->order.order);
  queryStatusSave(pRuntimeEnv, &qStatus);

  doScanAllDataBlocks(pRuntimeEnv);

  // set the correct start position, and load the corresponding block in buffer if required.
  TSKEY actKey = loadRequiredBlockIntoMem(pRuntimeEnv, &pRuntimeEnv->startPos);
  assert((QUERY_IS_ASC_QUERY(pQuery) && actKey >= pQuery->skey) ||
         (!QUERY_IS_ASC_QUERY(pQuery) && actKey <= pQuery->skey));

  queryStatusRestore(pRuntimeEnv, &qStatus);
  enableFunctForMasterScan(pRuntimeEnv, pQuery->order.order);
  SET_MASTER_SCAN_FLAG(pRuntimeEnv);
}

void setQueryStatus(SQuery *pQuery, int8_t status) {
  if (status == QUERY_NOT_COMPLETED) {
    pQuery->over = status;
  } else {
    // QUERY_NOT_COMPLETED is not compatible with any other status, so clear its position first
    pQuery->over &= (~QUERY_NOT_COMPLETED);
    pQuery->over |= status;
  }
}

void vnodeScanAllData(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  setQueryStatus(pQuery, QUERY_NOT_COMPLETED);

  /* store the start query position */
  savePointPosition(&pRuntimeEnv->startPos, pQuery->fileId, pQuery->slot, pQuery->pos);
  int64_t skey = pQuery->lastKey;
  
  while (1) {
    doScanAllDataBlocks(pRuntimeEnv);

    bool toContinue = true;

    if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
      // for each group result, call the finalize function for each column
      SSlidingWindowInfo *pSlidingWindowInfo = &pRuntimeEnv->swindowResInfo;

      for (int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
        SOutputRes *buf = &pSlidingWindowInfo->pResult[i];

        SWindowStatus *pStatus = &pSlidingWindowInfo->pStatus[i];
        if (!pStatus->closed) {
          continue;
        }

        setGroupOutputBuffer(pRuntimeEnv, buf);

        for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
          aAggs[pQuery->pSelectExpr[j].pBase.functionId].xNextStep(&pRuntimeEnv->pCtx[j]);
          SResultInfo *pResInfo = GET_RES_INFO(&pRuntimeEnv->pCtx[j]);

          toContinue &= (pResInfo->complete);
        }
      }
    } else {
      for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
        aAggs[pQuery->pSelectExpr[j].pBase.functionId].xNextStep(&pRuntimeEnv->pCtx[j]);
        SResultInfo *pResInfo = GET_RES_INFO(&pRuntimeEnv->pCtx[j]);

        toContinue &= (pResInfo->complete);
      }
    }

    if (toContinue) {
      break;
    }

    // set the correct start position, and load the corresponding block in buffer if required.
    TSKEY actKey = loadRequiredBlockIntoMem(pRuntimeEnv, &pRuntimeEnv->startPos);
    assert((QUERY_IS_ASC_QUERY(pQuery) && actKey >= pQuery->skey) ||
           (!QUERY_IS_ASC_QUERY(pQuery) && actKey <= pQuery->skey));

    setQueryStatus(pQuery, QUERY_NOT_COMPLETED);
    pQuery->lastKey = pQuery->skey;

    /* check if query is killed or not */
    if (isQueryKilled(pQuery)) {
      setQueryStatus(pQuery, QUERY_NO_DATA_TO_CHECK);
      return;
    }
  }

  int64_t newSkey = pQuery->skey;
  pQuery->skey = skey;
  
  doSingleMeterSupplementScan(pRuntimeEnv);
  
  // update the pQuery->skey/pQuery->ekey to limit the scan scope of sliding query during
  // supplementary scan
  pQuery->skey = newSkey;
}

void doFinalizeResult(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    // for each group result, call the finalize function for each column
    SSlidingWindowInfo *pSlidingWindowInfo = &pRuntimeEnv->swindowResInfo;
    if (isGroupbyNormalCol(pQuery->pGroupbyExpr)) {
      closeAllSlidingWindow(pSlidingWindowInfo);
    }
    
    for (int32_t i = 0; i < pSlidingWindowInfo->size; ++i) {
      SOutputRes *buf = &pSlidingWindowInfo->pResult[i];
      if (!slidingWindowClosed(pSlidingWindowInfo, i)) {
        continue;
      }
      
      setGroupOutputBuffer(pRuntimeEnv, buf);

      for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
        aAggs[pQuery->pSelectExpr[j].pBase.functionId].xFinalize(&pRuntimeEnv->pCtx[j]);
      }

      /*
       * set the number of output results for group by normal columns, the number of output rows usually is 1 except
       * the top and bottom query
       */
      buf->numOfRows = getNumOfResult(pRuntimeEnv);
    }

  } else {
    for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
      aAggs[pQuery->pSelectExpr[j].pBase.functionId].xFinalize(&pRuntimeEnv->pCtx[j]);
    }
  }
}

static bool hasMainOutput(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;

    if (functionId != TSDB_FUNC_TS && functionId != TSDB_FUNC_TAG && functionId != TSDB_FUNC_TAGPRJ) {
      return true;
    }
  }

  return false;
}

int64_t getNumOfResult(SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  bool    hasMainFunction = hasMainOutput(pQuery);

  int64_t maxOutput = 0;
  for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
    int32_t functionId = pQuery->pSelectExpr[j].pBase.functionId;

    /*
     * ts, tag, tagprj function can not decide the output number of current query
     * the number of output result is decided by main output
     */
    if (hasMainFunction &&
        (functionId == TSDB_FUNC_TS || functionId == TSDB_FUNC_TAG || functionId == TSDB_FUNC_TAGPRJ)) {
      continue;
    }

    SResultInfo *pResInfo = GET_RES_INFO(&pRuntimeEnv->pCtx[j]);
    if (pResInfo != NULL && maxOutput < pResInfo->numOfRes) {
      maxOutput = pResInfo->numOfRes;
    }
  }

  return maxOutput;
}

static int32_t getNextIntervalQueryRange(SMeterQuerySupportObj *pSupporter, SQueryRuntimeEnv *pRuntimeEnv,
                                         int64_t *skey, int64_t *ekey) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  int32_t factor = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  *skey = pRuntimeEnv->intervalWindow.skey + (pQuery->slidingTime * factor);
  *ekey = pRuntimeEnv->intervalWindow.ekey + (pQuery->slidingTime * factor);

  if (pQuery->slidingTime > 0) {
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      // the next sliding window is not contained in the query time range
      if (*skey < pSupporter->rawSKey) {
        *skey = pSupporter->rawSKey;
      }

      if (*skey > pSupporter->rawEKey) {
        return QUERY_COMPLETED;
      }

      if (*ekey > pSupporter->rawEKey) {
        *ekey = pSupporter->rawEKey;
      }
    } else {
      if (*skey > pSupporter->rawSKey) {
        *skey = pSupporter->rawSKey;
      }

      if (*skey < pSupporter->rawEKey) {
        return QUERY_COMPLETED;
      }

      if (*ekey < pSupporter->rawEKey) {
        *ekey = pSupporter->rawEKey;
      }
    }
  }

  return QUERY_NOT_COMPLETED;
}

/*
 * forward the query range for next interval query
 */
void forwardIntervalQueryRange(SMeterQuerySupportObj *pSupporter, SQueryRuntimeEnv *pRuntimeEnv) {
  SQuery *pQuery = pRuntimeEnv->pQuery;
  if (pQuery->slidingTime > 0 && pQuery->nAggTimeInterval > 0) {
    if ((QUERY_IS_ASC_QUERY(pQuery) && pQuery->lastKey >= pQuery->ekey) ||
        (!QUERY_IS_ASC_QUERY(pQuery) && pQuery->lastKey <= pQuery->ekey)) {
      setQueryStatus(pQuery, QUERY_COMPLETED);
    } else {
      TSKEY nextTimestamp = loadRequiredBlockIntoMem(pRuntimeEnv, &pRuntimeEnv->nextPos);
    }

    return;
  }

  int32_t r = getNextIntervalQueryRange(pSupporter, pRuntimeEnv, &pQuery->skey, &pQuery->ekey);
  if (r == QUERY_COMPLETED) {
    setQueryStatus(pQuery, QUERY_COMPLETED);
    return;
  }

  getNextLogicalQueryRange(pRuntimeEnv, &pRuntimeEnv->intervalWindow);

  /* ensure the search in cache will return right position */
  pQuery->lastKey = pQuery->skey;

  TSKEY nextTimestamp = loadRequiredBlockIntoMem(pRuntimeEnv, &pRuntimeEnv->nextPos);
  if ((nextTimestamp > pSupporter->rawEKey && QUERY_IS_ASC_QUERY(pQuery)) ||
      (nextTimestamp < pSupporter->rawEKey && !QUERY_IS_ASC_QUERY(pQuery)) ||
      Q_STATUS_EQUAL(pQuery->over, QUERY_NO_DATA_TO_CHECK)) {
    setQueryStatus(pQuery, QUERY_COMPLETED);
    return;
  }

  // bridge the gap in group by time function
  if ((nextTimestamp > pQuery->ekey && QUERY_IS_ASC_QUERY(pQuery)) ||
      (nextTimestamp < pQuery->ekey && !QUERY_IS_ASC_QUERY(pQuery))) {
    getAlignedIntervalQueryRange(pRuntimeEnv, nextTimestamp, pSupporter->rawSKey, pSupporter->rawEKey);
  }
}

static int32_t offsetComparator(const void *pLeft, const void *pRight) {
  SMeterDataInfo **pLeft1 = (SMeterDataInfo **)pLeft;
  SMeterDataInfo **pRight1 = (SMeterDataInfo **)pRight;

  if ((*pLeft1)->offsetInHeaderFile == (*pRight1)->offsetInHeaderFile) {
    return 0;
  }

  return ((*pLeft1)->offsetInHeaderFile > (*pRight1)->offsetInHeaderFile) ? 1 : -1;
}

/**
 *
 * @param pQInfo
 * @param fid
 * @param pQueryFileInfo
 * @param start
 * @param end
 * @param pMeterHeadDataInfo
 * @return
 */
int32_t vnodeFilterQualifiedMeters(SQInfo *pQInfo, int32_t vid, tSidSet *pSidSet, SMeterDataInfo *pMeterDataInfo,
                                   int32_t *numOfMeters, SMeterDataInfo ***pReqMeterDataInfo) {
  SQuery *pQuery = &pQInfo->query;

  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;
  SMeterSidExtInfo **    pMeterSidExtInfo = pSupporter->pMeterSidExtInfo;
  SQueryRuntimeEnv *     pRuntimeEnv = &pSupporter->runtimeEnv;

  SVnodeObj *pVnode = &vnodeList[vid];

  char *buf = calloc(1, getCompHeaderSegSize(&pVnode->cfg));
  if (buf == NULL) {
    *numOfMeters = 0;
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  SQueryFilesInfo *pVnodeFileInfo = &pRuntimeEnv->vnodeFileInfo;

  int32_t headerSize = getCompHeaderSegSize(&pVnode->cfg);
  lseek(pVnodeFileInfo->headerFd, TSDB_FILE_HEADER_LEN, SEEK_SET);
  read(pVnodeFileInfo->headerFd, buf, headerSize);

  // check the offset value integrity
  if (validateHeaderOffsetSegment(pQInfo, pRuntimeEnv->vnodeFileInfo.headerFilePath, vid, buf - TSDB_FILE_HEADER_LEN,
                                  headerSize) < 0) {
    free(buf);
    *numOfMeters = 0;

    return TSDB_CODE_FILE_CORRUPTED;
  }

  int64_t oldestKey = getOldestKey(pVnode->numOfFiles, pVnode->fileId, &pVnode->cfg);
  (*pReqMeterDataInfo) = malloc(POINTER_BYTES * pSidSet->numOfSids);
  if (*pReqMeterDataInfo == NULL) {
    free(buf);
    *numOfMeters = 0;

    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  int32_t groupId = 0;
  TSKEY   skey, ekey;

  for (int32_t i = 0; i < pSidSet->numOfSids; ++i) {  // load all meter meta info
    SMeterObj *pMeterObj = getMeterObj(pSupporter->pMetersHashTable, pMeterSidExtInfo[i]->sid);
    if (pMeterObj == NULL) {
      dError("QInfo:%p failed to find required sid:%d", pQInfo, pMeterSidExtInfo[i]->sid);
      continue;
    }

    if (i >= pSidSet->starterPos[groupId + 1]) {
      groupId += 1;
    }

    SMeterDataInfo *pOneMeterDataInfo = &pMeterDataInfo[i];
    if (pOneMeterDataInfo->pMeterObj == NULL) {
      setMeterDataInfo(pOneMeterDataInfo, pMeterObj, i, groupId);
    }

    /* restore possible exists new query range for this meter, which starts from cache */
    if (pOneMeterDataInfo->pMeterQInfo != NULL) {
      skey = pOneMeterDataInfo->pMeterQInfo->lastKey;
    } else {
      skey = pSupporter->rawSKey;
    }

    // query on disk data files, which actually starts from the lastkey
    ekey = pSupporter->rawEKey;

    if (QUERY_IS_ASC_QUERY(pQuery)) {
      assert(skey >= pSupporter->rawSKey);
      if (ekey < oldestKey || skey > pMeterObj->lastKeyOnFile) {
        continue;
      }
    } else {
      assert(skey <= pSupporter->rawSKey);
      if (skey < oldestKey || ekey > pMeterObj->lastKeyOnFile) {
        continue;
      }
    }

    int64_t      headerOffset = sizeof(SCompHeader) * pMeterObj->sid;
    SCompHeader *compHeader = (SCompHeader *)(buf + headerOffset);
    if (compHeader->compInfoOffset == 0) {  // current table is empty
      continue;
    }

    // corrupted file may cause the invalid compInfoOffset, check needs
    int32_t compHeaderOffset = getCompHeaderStartPosition(&pVnode->cfg);
    if (validateCompBlockOffset(pQInfo, pMeterObj, compHeader, &pRuntimeEnv->vnodeFileInfo, compHeaderOffset) !=
        TSDB_CODE_SUCCESS) {
      free(buf);
      *numOfMeters = 0;

      return TSDB_CODE_FILE_CORRUPTED;
    }

    pOneMeterDataInfo->offsetInHeaderFile = (uint64_t)compHeader->compInfoOffset;

    if (pOneMeterDataInfo->pMeterQInfo == NULL) {
      pOneMeterDataInfo->pMeterQInfo = createMeterQueryInfo(pQuery, pSupporter->rawSKey, pSupporter->rawEKey);
    }

    (*pReqMeterDataInfo)[*numOfMeters] = pOneMeterDataInfo;
    (*numOfMeters) += 1;
  }

  assert(*numOfMeters <= pSidSet->numOfSids);

  /* enable sequentially access*/
  if (*numOfMeters > 1) {
    qsort((*pReqMeterDataInfo), *numOfMeters, POINTER_BYTES, offsetComparator);
  }

  free(buf);

  return TSDB_CODE_SUCCESS;
}

SMeterQueryInfo *createMeterQueryInfo(SQuery *pQuery, TSKEY skey, TSKEY ekey) {
  SMeterQueryInfo *pMeterQueryInfo = calloc(1, sizeof(SMeterQueryInfo));

  pMeterQueryInfo->skey = skey;
  pMeterQueryInfo->ekey = ekey;
  pMeterQueryInfo->lastKey = skey;

  pMeterQueryInfo->numOfPages = 0;
  pMeterQueryInfo->numOfAlloc = INIT_ALLOCATE_DISK_PAGES;
  pMeterQueryInfo->pageList = calloc(pMeterQueryInfo->numOfAlloc, sizeof(uint32_t));
  pMeterQueryInfo->lastResRows = 0;

  pMeterQueryInfo->cur.vnodeIndex = -1;

  pMeterQueryInfo->resultInfo = calloc((size_t)pQuery->numOfOutputCols, sizeof(SResultInfo));
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    SResultInfo *pResInfo = &pMeterQueryInfo->resultInfo[i];
    setResultInfoBuf(pResInfo, pQuery->pSelectExpr[i].interResBytes, true);
  }

  return pMeterQueryInfo;
}

void destroyMeterQueryInfo(SMeterQueryInfo *pMeterQueryInfo, int32_t numOfCols) {
  if (pMeterQueryInfo == NULL) {
    return;
  }

  free(pMeterQueryInfo->pageList);
  for (int32_t i = 0; i < numOfCols; ++i) {
    tfree(pMeterQueryInfo->resultInfo[i].interResultBuf);
  }

  free(pMeterQueryInfo->resultInfo);
  free(pMeterQueryInfo);
}

void changeMeterQueryInfoForSuppleQuery(SMeterQueryInfo *pMeterQueryInfo, TSKEY skey, TSKEY ekey) {
  if (pMeterQueryInfo == NULL) {
    return;
  }

  pMeterQueryInfo->skey = skey;
  pMeterQueryInfo->ekey = ekey;
  pMeterQueryInfo->lastKey = pMeterQueryInfo->skey;

  pMeterQueryInfo->queryRangeSet = 0;
  pMeterQueryInfo->cur.order = pMeterQueryInfo->cur.order ^ 1;
  pMeterQueryInfo->cur.vnodeIndex = -1;

  // previous does not generate any results
  if (pMeterQueryInfo->numOfPages == 0) {
    pMeterQueryInfo->reverseFillRes = 0;
  } else {
    pMeterQueryInfo->reverseIndex = pMeterQueryInfo->numOfRes;
    pMeterQueryInfo->reverseFillRes = 1;
  }
}

static tFilePage *allocNewPage(SQuery *pQuery, SMeterQuerySupportObj *pSupporter, uint32_t *pageId) {
  if (pSupporter->lastPageId == pSupporter->numOfPages - 1) {
    if (extendDiskBuf(pQuery, pSupporter, pSupporter->numOfPages + pSupporter->numOfMeters) != TSDB_CODE_SUCCESS) {
      return NULL;
    }
  }

  *pageId = (++pSupporter->lastPageId);
  return getFilePage(pSupporter, *pageId);
}

tFilePage *addDataPageForMeterQueryInfo(SQuery *pQuery, SMeterQueryInfo *pMeterQueryInfo,
                                        SMeterQuerySupportObj *pSupporter) {
  uint32_t pageId = 0;

  tFilePage *pPage = allocNewPage(pQuery, pSupporter, &pageId);
  if (pPage == NULL) {  // failed to allocate disk-based buffer for intermediate results
    return NULL;
  }

  if (pMeterQueryInfo->numOfPages >= pMeterQueryInfo->numOfAlloc) {
    pMeterQueryInfo->numOfAlloc = pMeterQueryInfo->numOfAlloc << 1;
    pMeterQueryInfo->pageList = realloc(pMeterQueryInfo->pageList, sizeof(uint32_t) * pMeterQueryInfo->numOfAlloc);
  }

  pMeterQueryInfo->pageList[pMeterQueryInfo->numOfPages++] = pageId;
  return pPage;
}

void saveIntervalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, SMeterQueryInfo *pMeterQueryInfo) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  pMeterQueryInfo->skey = pQuery->skey;
  pMeterQueryInfo->ekey = pQuery->ekey;
  pMeterQueryInfo->lastKey = pQuery->lastKey;

  assert(((pQuery->lastKey >= pQuery->skey) && QUERY_IS_ASC_QUERY(pQuery)) ||
         ((pQuery->lastKey <= pQuery->skey) && !QUERY_IS_ASC_QUERY(pQuery)));

  if (pRuntimeEnv->pTSBuf != NULL) {
    pMeterQueryInfo->cur = tsBufGetCursor(pRuntimeEnv->pTSBuf);
  }
}

void restoreIntervalQueryRange(SQueryRuntimeEnv *pRuntimeEnv, SMeterQueryInfo *pMeterQueryInfo) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  pQuery->skey = pMeterQueryInfo->skey;
  pQuery->ekey = pMeterQueryInfo->ekey;
  pQuery->lastKey = pMeterQueryInfo->lastKey;

  assert(((pQuery->lastKey >= pQuery->skey) && QUERY_IS_ASC_QUERY(pQuery)) ||
         ((pQuery->lastKey <= pQuery->skey) && !QUERY_IS_ASC_QUERY(pQuery)));
}

static void clearAllMeterDataBlockInfo(SMeterDataInfo **pMeterDataInfo, int32_t start, int32_t end) {
  for (int32_t i = start; i < end; ++i) {
    tfree(pMeterDataInfo[i]->pBlock);
    pMeterDataInfo[i]->numOfBlocks = 0;
    pMeterDataInfo[i]->start = -1;
  }
}

static bool getValidDataBlocksRangeIndex(SMeterDataInfo *pMeterDataInfo, SQuery *pQuery, SCompBlock *pCompBlock,
                                         int64_t numOfBlocks, TSKEY minval, TSKEY maxval, int32_t *end) {
  SMeterObj *pMeterObj = pMeterDataInfo->pMeterObj;
  SQInfo *   pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  /*
   * search the possible blk that may satisfy the query condition always start from the min value, therefore,
   * the order is always ascending order
   */
  pMeterDataInfo->start = binarySearchForBlockImpl(pCompBlock, (int32_t)numOfBlocks, minval, TSQL_SO_ASC);
  if (minval > pCompBlock[pMeterDataInfo->start].keyLast || maxval < pCompBlock[pMeterDataInfo->start].keyFirst) {
    dTrace("QInfo:%p vid:%d sid:%d id:%s, no result in files", pQInfo, pMeterObj->vnode, pMeterObj->sid,
           pMeterObj->meterId);
    return false;
  }

  // incremental checks following blocks until whose time range does not overlap with the query range
  *end = pMeterDataInfo->start;
  while (*end <= (numOfBlocks - 1)) {
    if (pCompBlock[*end].keyFirst <= maxval && pCompBlock[*end].keyLast >= maxval) {
      break;
    }

    if (pCompBlock[*end].keyFirst > maxval) {
      *end -= 1;
      break;
    }

    if (*end == numOfBlocks - 1) {
      break;
    } else {
      ++(*end);
    }
  }

  return true;
}

static bool setValidDataBlocks(SMeterDataInfo *pMeterDataInfo, int32_t end) {
  int32_t size = (end - pMeterDataInfo->start) + 1;
  assert(size > 0);

  if (size != pMeterDataInfo->numOfBlocks) {
    memmove(pMeterDataInfo->pBlock, &pMeterDataInfo->pBlock[pMeterDataInfo->start], size * sizeof(SCompBlock));

    char *tmp = realloc(pMeterDataInfo->pBlock, size * sizeof(SCompBlock));
    if (tmp == NULL) {
      return false;
    }

    pMeterDataInfo->pBlock = (SCompBlock *)tmp;
    pMeterDataInfo->numOfBlocks = size;
  }

  return true;
}

static bool setCurrentQueryRange(SMeterDataInfo *pMeterDataInfo, SQuery *pQuery, TSKEY endKey, TSKEY *minval,
                                 TSKEY *maxval) {
  SQInfo *         pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);
  SMeterObj *      pMeterObj = pMeterDataInfo->pMeterObj;
  SMeterQueryInfo *pMeterQInfo = pMeterDataInfo->pMeterQInfo;

  if (QUERY_IS_ASC_QUERY(pQuery)) {
    *minval = pMeterQInfo->lastKey;
    *maxval = endKey;
  } else {
    *minval = endKey;
    *maxval = pMeterQInfo->lastKey;
  }

  if (*minval > *maxval) {
    qTrace("QInfo:%p vid:%d sid:%d id:%s, no result in files, qrange:%" PRId64 "-%" PRId64 ", lastKey:%" PRId64, pQInfo,
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pMeterQInfo->skey, pMeterQInfo->ekey,
           pMeterQInfo->lastKey);
    return false;
  } else {
    qTrace("QInfo:%p vid:%d sid:%d id:%s, query in files, qrange:%" PRId64 "-%" PRId64 ", lastKey:%" PRId64, pQInfo,
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pMeterQInfo->skey, pMeterQInfo->ekey,
           pMeterQInfo->lastKey);
    return true;
  }
}

/**
 * @param pSupporter
 * @param pQuery
 * @param numOfMeters
 * @param filePath
 * @param pMeterDataInfo
 * @return
 */
int32_t getDataBlocksForMeters(SMeterQuerySupportObj *pSupporter, SQuery *pQuery, int32_t numOfMeters,
                               const char *filePath, SMeterDataInfo **pMeterDataInfo, uint32_t *numOfBlocks) {
  SQInfo *           pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);
  SQueryCostSummary *pSummary = &pSupporter->runtimeEnv.summary;

  TSKEY minval, maxval;

  *numOfBlocks = 0;
  SQueryFilesInfo *pVnodeFileInfo = &pSupporter->runtimeEnv.vnodeFileInfo;

  // sequentially scan this header file to extract the compHeader info
  for (int32_t j = 0; j < numOfMeters; ++j) {
    SMeterObj *pMeterObj = pMeterDataInfo[j]->pMeterObj;

    lseek(pVnodeFileInfo->headerFd, pMeterDataInfo[j]->offsetInHeaderFile, SEEK_SET);

    SCompInfo compInfo = {0};
    read(pVnodeFileInfo->headerFd, &compInfo, sizeof(SCompInfo));

    int32_t ret = validateCompBlockInfoSegment(pQInfo, filePath, pMeterObj->vnode, &compInfo,
                                               pMeterDataInfo[j]->offsetInHeaderFile);
    if (ret != TSDB_CODE_SUCCESS) {  // file corrupted
      clearAllMeterDataBlockInfo(pMeterDataInfo, 0, numOfMeters);
      return TSDB_CODE_FILE_CORRUPTED;
    }

    if (compInfo.numOfBlocks <= 0 || compInfo.uid != pMeterDataInfo[j]->pMeterObj->uid) {
      clearAllMeterDataBlockInfo(pMeterDataInfo, 0, numOfMeters);
      continue;
    }

    int32_t size = compInfo.numOfBlocks * sizeof(SCompBlock);
    size_t  bufferSize = size + sizeof(TSCKSUM);

    pMeterDataInfo[j]->numOfBlocks = compInfo.numOfBlocks;
    char* p = realloc(pMeterDataInfo[j]->pBlock, bufferSize);
    if (p == NULL) {
      clearAllMeterDataBlockInfo(pMeterDataInfo, 0, numOfMeters);
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    } else {
      memset(p, 0, bufferSize);
      pMeterDataInfo[j]->pBlock = (SCompBlock*) p;
    }

    read(pVnodeFileInfo->headerFd, pMeterDataInfo[j]->pBlock, bufferSize);
    TSCKSUM checksum = *(TSCKSUM *)((char *)pMeterDataInfo[j]->pBlock + size);

    int64_t st = taosGetTimestampUs();

    // check compblock integrity
    ret = validateCompBlockSegment(pQInfo, filePath, &compInfo, (char *)pMeterDataInfo[j]->pBlock, pMeterObj->vnode,
                                   checksum);
    if (ret != TSDB_CODE_SUCCESS) {
      clearAllMeterDataBlockInfo(pMeterDataInfo, 0, numOfMeters);
      return TSDB_CODE_FILE_CORRUPTED;
    }

    int64_t et = taosGetTimestampUs();

    pSummary->readCompInfo++;
    pSummary->totalCompInfoSize += (size + sizeof(SCompInfo) + sizeof(TSCKSUM));
    pSummary->loadCompInfoUs += (et - st);

    if (!setCurrentQueryRange(pMeterDataInfo[j], pQuery, pSupporter->rawEKey, &minval, &maxval)) {
      clearAllMeterDataBlockInfo(pMeterDataInfo, j, j + 1);
      continue;
    }

    int32_t end = 0;
    if (!getValidDataBlocksRangeIndex(pMeterDataInfo[j], pQuery, pMeterDataInfo[j]->pBlock, compInfo.numOfBlocks,
                                      minval, maxval, &end)) {
      // current table has no qualified data blocks, erase its information.
      clearAllMeterDataBlockInfo(pMeterDataInfo, j, j + 1);
      continue;
    }

    if (!setValidDataBlocks(pMeterDataInfo[j], end)) {
      clearAllMeterDataBlockInfo(pMeterDataInfo, 0, numOfMeters);
      
      pQInfo->killed = 1;  // set query kill, abort current query since no memory available
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }

    qTrace("QInfo:%p vid:%d sid:%d id:%s, startIndex:%d, %d blocks qualified", pQInfo, pMeterObj->vnode, pMeterObj->sid,
           pMeterObj->meterId, pMeterDataInfo[j]->start, pMeterDataInfo[j]->numOfBlocks);

    (*numOfBlocks) += pMeterDataInfo[j]->numOfBlocks;
  }

  return TSDB_CODE_SUCCESS;
}

static void freeDataBlockFieldInfo(SMeterDataBlockInfoEx *pDataBlockInfoEx, int32_t len) {
  for (int32_t i = 0; i < len; ++i) {
    tfree(pDataBlockInfoEx[i].pBlock.fields);
  }
}

void freeMeterBlockInfoEx(SMeterDataBlockInfoEx *pDataBlockInfoEx, int32_t len) {
  freeDataBlockFieldInfo(pDataBlockInfoEx, len);
  tfree(pDataBlockInfoEx);
}

typedef struct SBlockOrderSupporter {
  int32_t                 numOfMeters;
  SMeterDataBlockInfoEx **pDataBlockInfoEx;
  int32_t *               blockIndexArray;
  int32_t *               numOfBlocksPerMeter;
} SBlockOrderSupporter;

static int32_t blockAccessOrderComparator(const void *pLeft, const void *pRight, void *param) {
  int32_t leftTableIndex = *(int32_t *)pLeft;
  int32_t rightTableIndex = *(int32_t *)pRight;

  SBlockOrderSupporter *pSupporter = (SBlockOrderSupporter *)param;

  int32_t leftTableBlockIndex = pSupporter->blockIndexArray[leftTableIndex];
  int32_t rightTableBlockIndex = pSupporter->blockIndexArray[rightTableIndex];

  if (leftTableBlockIndex > pSupporter->numOfBlocksPerMeter[leftTableIndex]) {
    /* left block is empty */
    return 1;
  } else if (rightTableBlockIndex > pSupporter->numOfBlocksPerMeter[rightTableIndex]) {
    /* right block is empty */
    return -1;
  }

  SMeterDataBlockInfoEx *pLeftBlockInfoEx = &pSupporter->pDataBlockInfoEx[leftTableIndex][leftTableBlockIndex];
  SMeterDataBlockInfoEx *pRightBlockInfoEx = &pSupporter->pDataBlockInfoEx[rightTableIndex][rightTableBlockIndex];

  //    assert(pLeftBlockInfoEx->pBlock.compBlock->offset != pRightBlockInfoEx->pBlock.compBlock->offset);
  if (pLeftBlockInfoEx->pBlock.compBlock->offset == pRightBlockInfoEx->pBlock.compBlock->offset &&
      pLeftBlockInfoEx->pBlock.compBlock->last == pRightBlockInfoEx->pBlock.compBlock->last) {
    // todo add more information
    dError("error in header file, two block with same offset:%p", pLeftBlockInfoEx->pBlock.compBlock->offset);
  }

  return pLeftBlockInfoEx->pBlock.compBlock->offset > pRightBlockInfoEx->pBlock.compBlock->offset ? 1 : -1;
}

void cleanBlockOrderSupporter(SBlockOrderSupporter *pSupporter, int32_t numOfTables) {
  tfree(pSupporter->numOfBlocksPerMeter);
  tfree(pSupporter->blockIndexArray);

  for (int32_t i = 0; i < numOfTables; ++i) {
    tfree(pSupporter->pDataBlockInfoEx[i]);
  }

  tfree(pSupporter->pDataBlockInfoEx);
}

int32_t createDataBlocksInfoEx(SMeterDataInfo **pMeterDataInfo, int32_t numOfMeters,
                               SMeterDataBlockInfoEx **pDataBlockInfoEx, int32_t numOfCompBlocks,
                               int32_t *numOfAllocBlocks, int64_t addr) {
  // release allocated memory first
  freeDataBlockFieldInfo(*pDataBlockInfoEx, *numOfAllocBlocks);

  if (*numOfAllocBlocks == 0 || *numOfAllocBlocks < numOfCompBlocks) {
    char *tmp = realloc((*pDataBlockInfoEx), sizeof(SMeterDataBlockInfoEx) * numOfCompBlocks);
    if (tmp == NULL) {
      tfree(*pDataBlockInfoEx);
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }

    *pDataBlockInfoEx = (SMeterDataBlockInfoEx *)tmp;
    memset((*pDataBlockInfoEx), 0, sizeof(SMeterDataBlockInfoEx) * numOfCompBlocks);
    *numOfAllocBlocks = numOfCompBlocks;
  }

  SBlockOrderSupporter supporter = {0};
  supporter.numOfMeters = numOfMeters;
  supporter.numOfBlocksPerMeter = calloc(1, sizeof(int32_t) * numOfMeters);
  supporter.blockIndexArray = calloc(1, sizeof(int32_t) * numOfMeters);
  supporter.pDataBlockInfoEx = calloc(1, POINTER_BYTES * numOfMeters);

  if (supporter.numOfBlocksPerMeter == NULL || supporter.blockIndexArray == NULL ||
      supporter.pDataBlockInfoEx == NULL) {
    cleanBlockOrderSupporter(&supporter, 0);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  int32_t cnt = 0;
  int32_t numOfQualMeters = 0;
  for (int32_t j = 0; j < numOfMeters; ++j) {
    if (pMeterDataInfo[j]->numOfBlocks == 0) {
      continue;
    }

    SCompBlock *pBlock = pMeterDataInfo[j]->pBlock;
    supporter.numOfBlocksPerMeter[numOfQualMeters] = pMeterDataInfo[j]->numOfBlocks;

    char *buf = calloc(1, sizeof(SMeterDataBlockInfoEx) * pMeterDataInfo[j]->numOfBlocks);
    if (buf == NULL) {
      cleanBlockOrderSupporter(&supporter, numOfQualMeters);
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }

    supporter.pDataBlockInfoEx[numOfQualMeters] = (SMeterDataBlockInfoEx *)buf;

    for (int32_t k = 0; k < pMeterDataInfo[j]->numOfBlocks; ++k) {
      SMeterDataBlockInfoEx *pBlockInfoEx = &supporter.pDataBlockInfoEx[numOfQualMeters][k];
  
      pBlockInfoEx->pBlock.compBlock = &pBlock[k];
      pBlockInfoEx->pBlock.fields = NULL;
  
      pBlockInfoEx->pMeterDataInfo = pMeterDataInfo[j];
      pBlockInfoEx->groupIdx = pMeterDataInfo[j]->groupIdx;     // set the group index
      pBlockInfoEx->blockIndex = pMeterDataInfo[j]->start + k;  // set the block index in original meter
      cnt++;
    }

    numOfQualMeters++;
  }

  dTrace("QInfo %p create data blocks info struct completed", addr);

  assert(cnt == numOfCompBlocks && numOfQualMeters <= numOfMeters);  // the pMeterDataInfo[j]->numOfBlocks may be 0
  supporter.numOfMeters = numOfQualMeters;
  SLoserTreeInfo *pTree = NULL;

  uint8_t ret = tLoserTreeCreate(&pTree, supporter.numOfMeters, &supporter, blockAccessOrderComparator);
  if (ret != TSDB_CODE_SUCCESS) {
    cleanBlockOrderSupporter(&supporter, numOfMeters);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  int32_t numOfTotal = 0;

  while (numOfTotal < cnt) {
    int32_t                pos = pTree->pNode[0].index;
    SMeterDataBlockInfoEx *pBlocksInfoEx = supporter.pDataBlockInfoEx[pos];
    int32_t                index = supporter.blockIndexArray[pos]++;

    (*pDataBlockInfoEx)[numOfTotal++] = pBlocksInfoEx[index];

    // set data block index overflow, in order to disable the offset comparator
    if (supporter.blockIndexArray[pos] >= supporter.numOfBlocksPerMeter[pos]) {
      supporter.blockIndexArray[pos] = supporter.numOfBlocksPerMeter[pos] + 1;
    }

    tLoserTreeAdjust(pTree, pos + supporter.numOfMeters);
  }

  /*
   * available when no import exists
   * for(int32_t i = 0; i < cnt - 1; ++i) {
   *   assert((*pDataBlockInfoEx)[i].pBlock.compBlock->offset < (*pDataBlockInfoEx)[i+1].pBlock.compBlock->offset);
   * }
   */

  dTrace("QInfo %p %d data blocks sort completed", addr, cnt);
  cleanBlockOrderSupporter(&supporter, numOfMeters);
  free(pTree);

  return TSDB_CODE_SUCCESS;
}

/**
 * set output buffer for different group
 * @param pRuntimeEnv
 * @param pDataBlockInfoEx
 */
void setExecutionContext(SMeterQuerySupportObj *pSupporter, SOutputRes *outputRes, int32_t meterIdx, int32_t groupIdx,
                         SMeterQueryInfo *pMeterQueryInfo) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;

  setGroupOutputBuffer(pRuntimeEnv, &outputRes[groupIdx]);
  initCtxOutputBuf(pRuntimeEnv);

  vnodeSetTagValueInParam(pSupporter->pSidSet, pRuntimeEnv, pSupporter->pMeterSidExtInfo[meterIdx]);

  // set the right cursor position for ts buffer
  if (pSupporter->runtimeEnv.pTSBuf != NULL) {
    if (pMeterQueryInfo->cur.vnodeIndex == -1) {
      pMeterQueryInfo->tag = pRuntimeEnv->pCtx[0].tag.i64Key;

      tsBufGetElemStartPos(pSupporter->runtimeEnv.pTSBuf, 0, pMeterQueryInfo->tag);
    } else {
      tsBufSetCursor(pSupporter->runtimeEnv.pTSBuf, &pMeterQueryInfo->cur);
    }
  }
}

static void setGroupOutputBuffer(SQueryRuntimeEnv *pRuntimeEnv, SOutputRes *pResult) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  // Note: pResult->result[i]->numOfElems == 0, there is only fixed number of results for each group
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    assert(pResult->result[i]->numOfElems == 0 || pResult->result[i]->numOfElems == 1);

    SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[i];
    pCtx->aOutputBuf = pResult->result[i]->data + pCtx->outputBytes * pResult->result[i]->numOfElems;

    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM || functionId == TSDB_FUNC_DIFF) {
      pCtx->ptsOutputBuf = pRuntimeEnv->pCtx[0].aOutputBuf;
    }

    /*
     * set the output buffer information and intermediate buffer
     * not all queries require the interResultBuf, such as COUNT
     */
    pCtx->resultInfo = &pResult->resultInfo[i];

    // set super table query flag
    SResultInfo *pResInfo = GET_RES_INFO(pCtx);
    if (!isGroupbyNormalCol(pQuery->pGroupbyExpr)) {
      pResInfo->superTableQ = true;
    }
  }
}

static char *getOutputResPos(SQueryRuntimeEnv *pRuntimeEnv, tFilePage *pData, int32_t row, int32_t col) {
  // the output for each record should be less than the DEFAULT_INTERN_BUF_SIZE
  assert(pRuntimeEnv->pCtx[col].outputBytes <= DEFAULT_INTERN_BUF_SIZE);

  return (char *)pData->data + pRuntimeEnv->offset[col] * pRuntimeEnv->numOfRowsPerPage +
         pRuntimeEnv->pCtx[col].outputBytes * row;
}

void setCtxOutputPointerForSupplementScan(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  int32_t    index = pMeterQueryInfo->reverseIndex;
  tFilePage *pData = NULL;
  int32_t    i = 0;

  // find the position for this output result
  for (; i < pMeterQueryInfo->numOfPages; ++i) {
    pData = getFilePage(pSupporter, pMeterQueryInfo->pageList[i]);
    if (index <= pData->numOfElems) {
      break;
    }
    index -= pData->numOfElems;
  }

  assert(index >= 0);

  /*
   * if it is the first records in master scan, no next results exist, so no need to init the result buffer
   * all data are processed and save to buffer during supplementary scan.
   */
  if (index == 0) {
    return;
  }

  for (int32_t k = 0; k < pQuery->numOfOutputCols; ++k) {
    SQLFunctionCtx *pCtx = &pRuntimeEnv->pCtx[k];

    pCtx->aOutputBuf = getOutputResPos(pRuntimeEnv, pData, index - 1, k);
    SResultInfo *pResInfo = GET_RES_INFO(pCtx);
    if (pResInfo->complete) {
      continue;
    }

    int32_t functId = pQuery->pSelectExpr[k].pBase.functionId;

    // setup runtime environment
    if ((QUERY_IS_ASC_QUERY(pQuery) && functId == TSDB_FUNC_FIRST_DST) ||
        (!QUERY_IS_ASC_QUERY(pQuery) && functId == TSDB_FUNC_LAST_DST)) {
      if (pMeterQueryInfo->lastResRows == 0) {
        pCtx->currentStage = 0;

        resetResultInfo(pResInfo);
        aAggs[functId].init(pCtx);
      }
    }
  }

  // the first column is always the timestamp for interval query
  TSKEY      ts = *(TSKEY *)pRuntimeEnv->pCtx[0].aOutputBuf;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;
  qTrace("QInfo:%p vid:%d sid:%d id:%s, set output result pointer, ts:%" PRId64 ", index:%d", GET_QINFO_ADDR(pQuery),
         pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, ts, pMeterQueryInfo->reverseIndex);
}

void validateTimestampForSupplementResult(SQueryRuntimeEnv *pRuntimeEnv, int64_t numOfIncrementRes) {
  SQuery *        pQuery = pRuntimeEnv->pQuery;
  SQLFunctionCtx *pCtx = pRuntimeEnv->pCtx;

  if (pRuntimeEnv->scanFlag == SUPPLEMENTARY_SCAN && numOfIncrementRes > 0) {
    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
      if (functionId == TSDB_FUNC_TS) {
        assert(*(TSKEY *)pCtx[i].aOutputBuf == pCtx[i].nStartQueryTimestamp);
      }
    }
  }
}

int32_t setOutputBufferForIntervalQuery(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  tFilePage *       pData = NULL;

  SQuery *pQuery = pRuntimeEnv->pQuery;

  // in the first scan, new space needed for results
  if (pMeterQueryInfo->numOfPages == 0) {
    pData = addDataPageForMeterQueryInfo(pQuery, pMeterQueryInfo, pSupporter);
  } else {
    int32_t lastPageId = pMeterQueryInfo->pageList[pMeterQueryInfo->numOfPages - 1];
    pData = getFilePage(pSupporter, lastPageId);

    if (pData->numOfElems >= pRuntimeEnv->numOfRowsPerPage) {
      pData = addDataPageForMeterQueryInfo(pRuntimeEnv->pQuery, pMeterQueryInfo, pSupporter);
      if (pData != NULL) {
        assert(pData->numOfElems == 0);  // number of elements must be 0 for new allocated buffer
      }
    }
  }

  if (pData == NULL) {
    return -1;
  }

  for (int32_t i = 0; i < pRuntimeEnv->pQuery->numOfOutputCols; ++i) {
    pRuntimeEnv->pCtx[i].aOutputBuf = getOutputResPos(pRuntimeEnv, pData, pData->numOfElems, i);
    pRuntimeEnv->pCtx[i].resultInfo = &pMeterQueryInfo->resultInfo[i];
  }

  return TSDB_CODE_SUCCESS;
}

int32_t setIntervalQueryExecutionContext(SMeterQuerySupportObj *pSupporter, int32_t meterIdx,
                                         SMeterQueryInfo *pMeterQueryInfo) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;

  if (IS_MASTER_SCAN(pRuntimeEnv)) {
    if (setOutputBufferForIntervalQuery(pSupporter, pMeterQueryInfo) != TSDB_CODE_SUCCESS) {
      // not enough disk space or memory buffer for intermediate results
      return -1;
    }

    if (pMeterQueryInfo->lastResRows == 0) {
      initCtxOutputBuf(pRuntimeEnv);
    }

  } else {
    if (pMeterQueryInfo->reverseFillRes) {
      setCtxOutputPointerForSupplementScan(pSupporter, pMeterQueryInfo);
    } else {
      /*
       * set output buffer for reverse scan data blocks
       * find the correct output position of existed results during
       *
       * If the master scan does not produce any results, new spaces needed to be allocated during supplement scan
       */
      if (setOutputBufferForIntervalQuery(pSupporter, pMeterQueryInfo) != TSDB_CODE_SUCCESS) {
        return -1;
      }
    }
  }

  vnodeSetTagValueInParam(pSupporter->pSidSet, pRuntimeEnv, pSupporter->pMeterSidExtInfo[meterIdx]);

  // both the master and supplement scan needs to set the correct ts comp start position
  if (pSupporter->runtimeEnv.pTSBuf != NULL) {
    if (pMeterQueryInfo->cur.vnodeIndex == -1) {
      pMeterQueryInfo->tag = pRuntimeEnv->pCtx[0].tag.i64Key;

      tsBufGetElemStartPos(pSupporter->runtimeEnv.pTSBuf, 0, pMeterQueryInfo->tag);

      // keep the cursor info of current meter
      pMeterQueryInfo->cur = pSupporter->runtimeEnv.pTSBuf->cur;
    } else {
      tsBufSetCursor(pSupporter->runtimeEnv.pTSBuf, &pMeterQueryInfo->cur);
    }
  }

  return 0;
}

static void doApplyIntervalQueryOnBlock(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo,
                                        SBlockInfo *pBlockInfo, int64_t *pPrimaryCol, SField *pFields,
                                        __block_search_fn_t searchFn) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;
  int32_t           factor = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);

  int64_t nextKey = -1;
  bool    queryCompleted = false;

  while (1) {
    int32_t numOfRes = 0;
    int32_t steps = applyFunctionsOnBlock(pRuntimeEnv, pBlockInfo, pPrimaryCol, pFields, searchFn, &numOfRes);
    assert(steps > 0);

    // NOTE: in case of stable query, only ONE(or ZERO) row of result generated for each query range
    if (pMeterQueryInfo->lastResRows == 0) {
      pMeterQueryInfo->lastResRows = numOfRes;
    } else {
      assert(pMeterQueryInfo->lastResRows == 1);
    }

    int32_t pos = pQuery->pos + steps * factor;

    // query does not reach the end of current block
    if ((pos < pBlockInfo->size && QUERY_IS_ASC_QUERY(pQuery)) || (pos >= 0 && !QUERY_IS_ASC_QUERY(pQuery))) {
      nextKey = pPrimaryCol[pos];
    } else {
      assert((pQuery->lastKey > pBlockInfo->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
             (pQuery->lastKey < pBlockInfo->keyFirst && !QUERY_IS_ASC_QUERY(pQuery)));
    }

    // all data satisfy current query are checked, query completed
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      queryCompleted = (nextKey > pQuery->ekey || pQuery->ekey <= pBlockInfo->keyLast);
    } else {
      queryCompleted = (nextKey < pQuery->ekey || pQuery->ekey >= pBlockInfo->keyFirst);
    }

    /*
     * 1. there may be more date that satisfy current query interval, other than
     *    current block, we need to try next data blocks
     * 2. query completed, since reaches the upper bound of the main query range
     */
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      if (pQuery->lastKey > pBlockInfo->keyLast || pQuery->lastKey > pSupporter->rawEKey ||
          nextKey > pSupporter->rawEKey) {
        /*
         * current interval query is completed, set query result flag closed and
         * try next data block if pQuery->ekey == pSupporter->rawEKey, whole query is completed
         */
        if (pQuery->lastKey > pBlockInfo->keyLast) {
          assert(pQuery->ekey >= pBlockInfo->keyLast);
        }

        if (pQuery->lastKey > pSupporter->rawEKey || nextKey > pSupporter->rawEKey) {
          /* whole query completed, save result and abort */
          assert(queryCompleted);
          saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);

          // save the pQuery->lastKey for retrieve data in cache, actually, there will be no qualified data in cache.
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
        } else if (pQuery->ekey == pBlockInfo->keyLast) {
          /* current interval query is completed, set the next query range on other data blocks if exist */
          int64_t prevEKey = pQuery->ekey;

          getAlignedIntervalQueryRange(pRuntimeEnv, pQuery->lastKey, pSupporter->rawSKey, pSupporter->rawEKey);
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);

          assert(queryCompleted && prevEKey < pQuery->skey);
          if (pMeterQueryInfo->lastResRows > 0) {
            saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
          }
        } else {
          /*
           * Data that satisfy current query range may locate in current block and blocks that are directly right
           * next to current block. Therefore, we need to keep the query range(interval) unchanged until reaching
           * the direct next data block, while only forwards the pQuery->lastKey.
           *
           * With the information of the directly next data block, whether locates in cache or disk,
           * current interval query being completed or not can be decided.
           */
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
          assert(pQuery->lastKey > pBlockInfo->keyLast && pQuery->lastKey <= pQuery->ekey);

          /*
           * if current block is the last block of current file, we still close the result flag, and
           * merge with other meters in the same group
           */
          if (queryCompleted) {
            saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
          }
        }

        break;
      }
    } else {
      if (pQuery->lastKey < pBlockInfo->keyFirst || pQuery->lastKey < pSupporter->rawEKey ||
          nextKey < pSupporter->rawEKey) {
        if (pQuery->lastKey < pBlockInfo->keyFirst) {
          assert(pQuery->ekey <= pBlockInfo->keyFirst);
        }

        if (pQuery->lastKey < pSupporter->rawEKey || (nextKey < pSupporter->rawEKey && nextKey != -1)) {
          /* whole query completed, save result and abort */
          assert(queryCompleted);
          saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);

          /*
           * save the pQuery->lastKey for retrieve data in cache, actually,
           * there will be no qualified data in cache.
           */
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
        } else if (pQuery->ekey == pBlockInfo->keyFirst) {
          // current interval query is completed, set the next query range on other data blocks if exist
          int64_t prevEKey = pQuery->ekey;

          getAlignedIntervalQueryRange(pRuntimeEnv, pQuery->lastKey, pSupporter->rawSKey, pSupporter->rawEKey);
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);

          assert(queryCompleted && prevEKey > pQuery->skey);
          if (pMeterQueryInfo->lastResRows > 0) {
            saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
          }
        } else {
          /*
           * Data that satisfy current query range may locate in current block and blocks that are
           * directly right next to current block. Therefore, we need to keep the query range(interval)
           * unchanged until reaching the direct next data block, while only forwards the pQuery->lastKey.
           *
           * With the information of the directly next data block, whether locates in cache or disk,
           * current interval query being completed or not can be decided.
           */
          saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
          assert(pQuery->lastKey < pBlockInfo->keyFirst && pQuery->lastKey >= pQuery->ekey);

          /*
           * if current block is the last block of current file, we still close the result
           * flag, and merge with other meters in the same group
           */
          if (queryCompleted) {
            saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
          }
        }

        break;
      }
    }

    assert(queryCompleted);
    saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);

    assert((nextKey >= pQuery->lastKey && QUERY_IS_ASC_QUERY(pQuery)) ||
           (nextKey <= pQuery->lastKey && !QUERY_IS_ASC_QUERY(pQuery)));

    /* still in the same block to query */
    getAlignedIntervalQueryRange(pRuntimeEnv, nextKey, pSupporter->rawSKey, pSupporter->rawEKey);
    saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);

    int32_t newPos = searchFn((char *)pPrimaryCol, pBlockInfo->size, pQuery->skey, pQuery->order.order);
    assert(newPos == pQuery->pos + steps * factor);

    pQuery->pos = newPos;
  }
}

static void doApplyIntervalQueryOnBlock_rv(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo,
                                        SBlockInfo *pBlockInfo, int64_t *pPrimaryCol, SField *pFields,
                                        __block_search_fn_t searchFn) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;
  int32_t           factor = GET_FORWARD_DIRECTION_FACTOR(pQuery->order.order);
  
  while (1) {
    int64_t nextKey = -1;
    int32_t numOfRes = 0;
    
    int32_t steps = applyFunctionsOnBlock(pRuntimeEnv, pBlockInfo, pPrimaryCol, pFields, searchFn, &numOfRes);
    assert(steps > 0);

    // NOTE: in case of stable query, only ONE(or ZERO) row of result generated for each query range
    if (pMeterQueryInfo->lastResRows == 0) {
      pMeterQueryInfo->lastResRows = numOfRes;
    } else {
      assert(pMeterQueryInfo->lastResRows == 1);
    }
  
    int32_t pos = pQuery->pos + steps * factor;
  
    // query does not reach the end of current block
    if ((pos < pBlockInfo->size && QUERY_IS_ASC_QUERY(pQuery)) || (pos >= 0 && !QUERY_IS_ASC_QUERY(pQuery))) {
      nextKey = pPrimaryCol[pos];
    } else {
      assert((pQuery->lastKey > pBlockInfo->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
          (pQuery->lastKey < pBlockInfo->keyFirst && !QUERY_IS_ASC_QUERY(pQuery)));
    }
    
    // all data satisfy current query are checked, query completed
    bool completed = false;
    if (QUERY_IS_ASC_QUERY(pQuery)) {
      completed = (pQuery->lastKey > pQuery->ekey);
    } else {
      completed = (pQuery->lastKey < pQuery->ekey);
    }
  
    /*
     * 1. there may be more date that satisfy current query interval, other than
     *    current block, we need to try next data blocks
     * 2. query completed, since reaches the upper bound of the main query range
     */
    if (!completed) {
      /*
       * Data that satisfy current query range may locate in current block and blocks that are directly right
       * next to current block. Therefore, we need to keep the query range(interval) unchanged until reaching
       * the direct next data block, while only forwards the pQuery->lastKey.
       *
       * With the information of the directly next data block, whether locates in cache or disk,
       * current interval query being completed or not can be decided.
       */
      saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
      
      if (QUERY_IS_ASC_QUERY(pQuery)) {
        assert(pQuery->lastKey > pBlockInfo->keyLast && pQuery->lastKey <= pQuery->ekey);
      } else {
        assert(pQuery->lastKey < pBlockInfo->keyFirst && pQuery->lastKey >= pQuery->ekey);
      }
      
      break;
    }
  
    assert(completed);
    
    if (pQuery->ekey == pSupporter->rawEKey) {
      /* whole query completed, save result and abort */
      saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
    
      // save the pQuery->lastKey for retrieve data in cache, actually, there will be no qualified data in cache.
      saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
      
      return;
    } else if ((QUERY_IS_ASC_QUERY(pQuery) && pQuery->ekey == pBlockInfo->keyLast) ||
        (!QUERY_IS_ASC_QUERY(pQuery) && pQuery->ekey == pBlockInfo->keyFirst)) {
      /* current interval query is completed, set the next query range on other data blocks if exist */
      saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
      return;
    }
  
    saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
    
    assert(pos >= 0 && pos < pBlockInfo->size);
    assert((nextKey >= pQuery->lastKey && QUERY_IS_ASC_QUERY(pQuery)) ||
        (nextKey <= pQuery->lastKey && !QUERY_IS_ASC_QUERY(pQuery)));
  
    /* still in the same block to query */
    getAlignedIntervalQueryRange(pRuntimeEnv, nextKey, pSupporter->rawSKey, pSupporter->rawEKey);
    saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
  
    int32_t newPos = searchFn((char *)pPrimaryCol, pBlockInfo->size, pQuery->skey, pQuery->order.order);
    assert(newPos == pQuery->pos + steps * factor);
  
    pQuery->pos = newPos;
  }
  
}
int64_t getNextAccessedKeyInData(SQuery *pQuery, int64_t *pPrimaryCol, SBlockInfo *pBlockInfo, int32_t blockStatus) {
  assert(pQuery->pos >= 0 && pQuery->pos <= pBlockInfo->size - 1);

  TSKEY key = -1;
  if (IS_DATA_BLOCK_LOADED(blockStatus)) {
    key = pPrimaryCol[pQuery->pos];
  } else {
    assert(pQuery->pos == pBlockInfo->size - 1 || pQuery->pos == 0);
    key = QUERY_IS_ASC_QUERY(pQuery) ? pBlockInfo->keyFirst : pBlockInfo->keyLast;
  }

  assert((key >= pQuery->skey && QUERY_IS_ASC_QUERY(pQuery)) || (key <= pQuery->skey && !QUERY_IS_ASC_QUERY(pQuery)));
  return key;
}

/*
 * There are two cases to handle:
 *
 * 1. Query range is not set yet (queryRangeSet = 0). we need to set the query range info, including pQuery->lastKey,
 *    pQuery->skey, and pQuery->eKey.
 * 2. Query range is set and query is in progress. There may be another result with the same query ranges to be
 *    merged during merge stage. In this case, we need the pMeterQueryInfo->lastResRows to decide if there
 *    is a previous result generated or not.
 */
void setIntervalQueryRange(SMeterQueryInfo *pMeterQueryInfo, SMeterQuerySupportObj *pSupporter, TSKEY key) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  if (pMeterQueryInfo->queryRangeSet) {
    assert((QUERY_IS_ASC_QUERY(pQuery) && pQuery->lastKey >= pQuery->skey) ||
           (!QUERY_IS_ASC_QUERY(pQuery) && pQuery->lastKey <= pQuery->skey));

    if ((pQuery->ekey < key && QUERY_IS_ASC_QUERY(pQuery)) || (pQuery->ekey > key && !QUERY_IS_ASC_QUERY(pQuery))) {
      /*
       * last query on this block of the meter is done, start next interval on this block
       * otherwise, keep the previous query range and proceed
       */
      getAlignedIntervalQueryRange(pRuntimeEnv, key, pSupporter->rawSKey, pSupporter->rawEKey);
      saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);

      // previous query does not be closed, save the results and close it
      if (pMeterQueryInfo->lastResRows > 0) {
        saveResult(pSupporter, pMeterQueryInfo, pMeterQueryInfo->lastResRows);
      }
    } else {
      /* current query not completed, continue. do nothing with respect to query range, */
    }
  } else {
    pQuery->skey = key;
    assert(pMeterQueryInfo->lastResRows == 0);

    if ((QUERY_IS_ASC_QUERY(pQuery) && (pQuery->ekey < pQuery->skey)) ||
        (!QUERY_IS_ASC_QUERY(pQuery) && (pQuery->skey < pQuery->ekey))) {
      // for too small query range, no data in this interval.
      return;
    }

    getAlignedIntervalQueryRange(pRuntimeEnv, pQuery->skey, pSupporter->rawSKey, pSupporter->rawEKey);
    saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
    pMeterQueryInfo->queryRangeSet = 1;
  }
}

bool requireTimestamp(SQuery *pQuery) {
  for (int32_t i = 0; i < pQuery->numOfOutputCols; i++) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;
    if ((aAggs[functionId].nStatus & TSDB_FUNCSTATE_NEED_TS) != 0) {
      return true;
    }
  }
  return false;
}

static void setTimestampRange(SQueryRuntimeEnv *pRuntimeEnv, int64_t stime, int64_t etime) {
  SQuery *pQuery = pRuntimeEnv->pQuery;

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    int32_t functionId = pQuery->pSelectExpr[i].pBase.functionId;

    if (functionId == TSDB_FUNC_SPREAD) {
      pRuntimeEnv->pCtx[i].param[1].dKey = stime;
      pRuntimeEnv->pCtx[i].param[2].dKey = etime;

      pRuntimeEnv->pCtx[i].param[1].nType = TSDB_DATA_TYPE_DOUBLE;
      pRuntimeEnv->pCtx[i].param[2].nType = TSDB_DATA_TYPE_DOUBLE;
    }
  }
}

bool needPrimaryTimestampCol(SQuery *pQuery, SBlockInfo *pBlockInfo) {
  /*
   * 1. if skey or ekey locates in this block, we need to load the timestamp column to decide the precise position
   * 2. if there are top/bottom, first_dst/last_dst functions, we need to load timestamp column in any cases;
   */
  bool loadPrimaryTS = (pQuery->lastKey >= pBlockInfo->keyFirst && pQuery->lastKey <= pBlockInfo->keyLast) ||
                       (pQuery->ekey >= pBlockInfo->keyFirst && pQuery->ekey <= pBlockInfo->keyLast) ||
                       requireTimestamp(pQuery);

  return loadPrimaryTS;
}

int32_t LoadDatablockOnDemand(SCompBlock *pBlock, SField **pFields, uint8_t *blkStatus, SQueryRuntimeEnv *pRuntimeEnv,
                              int32_t fileIdx, int32_t slotIdx, __block_search_fn_t searchFn, bool onDemand) {
  SQuery *   pQuery = pRuntimeEnv->pQuery;
  SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;

  TSKEY *primaryKeys = (TSKEY *)pRuntimeEnv->primaryColBuffer->data;

  pQuery->slot = slotIdx;
  pQuery->pos = QUERY_IS_ASC_QUERY(pQuery) ? 0 : pBlock->numOfPoints - 1;

  SET_FILE_BLOCK_FLAG(*blkStatus);
  SET_DATA_BLOCK_NOT_LOADED(*blkStatus);

  if (((pQuery->lastKey <= pBlock->keyFirst && pQuery->ekey >= pBlock->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
       (pQuery->ekey <= pBlock->keyFirst && pQuery->lastKey >= pBlock->keyLast && !QUERY_IS_ASC_QUERY(pQuery))) &&
      onDemand) {
    int32_t req = 0;
    if (pQuery->numOfFilterCols > 0) {
      req = BLK_DATA_ALL_NEEDED;
    } else {
      for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
        int32_t functID = pQuery->pSelectExpr[i].pBase.functionId;
        req |= aAggs[functID].dataReqFunc(&pRuntimeEnv->pCtx[i], pBlock->keyFirst, pBlock->keyLast,
                                          pQuery->pSelectExpr[i].pBase.colInfo.colId, *blkStatus);
      }

      if (pRuntimeEnv->pTSBuf > 0 || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
        req |= BLK_DATA_ALL_NEEDED;
      }
    }

    if (req == BLK_DATA_NO_NEEDED) {
      qTrace("QInfo:%p vid:%d sid:%d id:%s, slot:%d, data block ignored, brange:%" PRId64 "-%" PRId64 ", rows:%d",
             GET_QINFO_ADDR(pQuery), pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, pQuery->slot,
             pBlock->keyFirst, pBlock->keyLast, pBlock->numOfPoints);

      setTimestampRange(pRuntimeEnv, pBlock->keyFirst, pBlock->keyLast);
    } else if (req == BLK_DATA_FILEDS_NEEDED) {
      if (loadDataBlockFieldsInfo(pRuntimeEnv, pBlock, pFields) < 0) {
        return DISK_DATA_LOAD_FAILED;
      }
    } else {
      assert(req == BLK_DATA_ALL_NEEDED);
      goto _load_all;
    }
  } else {
  _load_all:
    if (loadDataBlockFieldsInfo(pRuntimeEnv, pBlock, pFields) < 0) {
      return DISK_DATA_LOAD_FAILED;
    }

    if ((pQuery->lastKey <= pBlock->keyFirst && pQuery->ekey >= pBlock->keyLast && QUERY_IS_ASC_QUERY(pQuery)) ||
        (pQuery->lastKey >= pBlock->keyLast && pQuery->ekey <= pBlock->keyFirst && !QUERY_IS_ASC_QUERY(pQuery))) {
      /*
       * if this block is completed included in the query range, do more filter operation
       * filter the data block according to the value filter condition.
       * no need to load the data block, continue for next block
       */
      if (!needToLoadDataBlock(pQuery, *pFields, pRuntimeEnv->pCtx, pBlock->numOfPoints)) {
#if defined(_DEBUG_VIEW)
        dTrace("QInfo:%p fileId:%d, slot:%d, block discarded by per-filter, ", GET_QINFO_ADDR(pQuery), pQuery->fileId,
               pQuery->slot);
#endif
        qTrace("QInfo:%p id:%s slot:%d, data block ignored by pre-filter, fields loaded, brange:%" PRId64 "-%" PRId64
               ", rows:%d",
               GET_QINFO_ADDR(pQuery), pMeterObj->meterId, pQuery->slot, pBlock->keyFirst, pBlock->keyLast,
               pBlock->numOfPoints);
        return DISK_DATA_DISCARDED;
      }
    }

    SBlockInfo binfo = getBlockBasicInfo(pRuntimeEnv, pBlock, BLK_FILE_BLOCK);
    bool       loadTS = needPrimaryTimestampCol(pQuery, &binfo);

    /*
     * the pRuntimeEnv->pMeterObj is not updated during loop, since which meter this block is belonged to is not matter
     * in order to enforce to load the data block, we HACK the load check procedure,
     * by changing pQuery->slot each time to IGNORE the pLoadInfo data check. It is NOT a normal way.
     */
    int32_t ret = loadDataBlockIntoMem(pBlock, pFields, pRuntimeEnv, fileIdx, loadTS, false);
    SET_DATA_BLOCK_LOADED(*blkStatus);

    if (ret < 0) {
      return DISK_DATA_LOAD_FAILED;
    }

    /* find first qualified record position in this block */
    if (loadTS) {
      /* find first qualified record position in this block */
      pQuery->pos =
          searchFn(pRuntimeEnv->primaryColBuffer->data, pBlock->numOfPoints, pQuery->lastKey, pQuery->order.order);
      /* boundary timestamp check */
      assert(pBlock->keyFirst == primaryKeys[0] && pBlock->keyLast == primaryKeys[pBlock->numOfPoints - 1]);
    }

    /*
     * NOTE:
     * if the query of current timestamp window is COMPLETED, the query range condition may not be satisfied
     * such as:
     * pQuery->lastKey + 1 == pQuery->ekey for descending order interval query
     * pQuery->lastKey - 1 == pQuery->ekey for ascending query
     */
    assert(((pQuery->ekey >= pQuery->lastKey || pQuery->ekey == pQuery->lastKey - 1) && QUERY_IS_ASC_QUERY(pQuery)) ||
           ((pQuery->ekey <= pQuery->lastKey || pQuery->ekey == pQuery->lastKey + 1) && !QUERY_IS_ASC_QUERY(pQuery)));
  }

  return DISK_DATA_LOADED;
}

bool onDemandLoadDatablock(SQuery *pQuery, int16_t queryRangeSet) {
  return (pQuery->nAggTimeInterval == 0) || ((queryRangeSet == 1) && (pQuery->nAggTimeInterval > 0));
}

static void validateResultBuf(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pSupporter->runtimeEnv.pQuery;

  tFilePage *newOutput = getFilePage(pSupporter, pMeterQueryInfo->pageList[pMeterQueryInfo->numOfPages - 1]);
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    assert(pRuntimeEnv->pCtx[i].aOutputBuf - newOutput->data < DEFAULT_INTERN_BUF_SIZE);
  }
}

int32_t saveResult(SMeterQuerySupportObj *pSupporter, SMeterQueryInfo *pMeterQueryInfo, int32_t numOfResult) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  // no results generated, do nothing for master scan
  if (numOfResult <= 0) {
    if (IS_MASTER_SCAN(pRuntimeEnv)) {
      return TSDB_CODE_SUCCESS;
    } else {
      /*
       * There is a case that no result generated during the the supplement scan, and during the main
       * scan also no result generated. The index can be backwards moved.
       *
       * However, if during the main scan, there is a result generated, such as applies count to timestamp, which
       * always generates a result, but applies last query to a NULL column may fail to generate results during the
       * supplement scan.
       *
       * NOTE:
       * nStartQueryTimestamp is the actually timestamp of current interval, if the actually interval timestamp
       * equals to the recorded timestamp that is acquired during the master scan, backwards one step even
       * there is no results during the supplementary scan.
       */
      TSKEY ts = *(TSKEY *)pRuntimeEnv->pCtx[0].aOutputBuf;
      if (ts == pRuntimeEnv->pCtx[0].nStartQueryTimestamp && pMeterQueryInfo->reverseIndex > 0) {
        assert(pMeterQueryInfo->numOfRes >= 0 && pMeterQueryInfo->reverseIndex > 0 &&
               pMeterQueryInfo->reverseIndex <= pMeterQueryInfo->numOfRes);

        // backward one step from the previous position, the start position is (pMeterQueryInfo->numOfRows-1);
        pMeterQueryInfo->reverseIndex -= 1;
        setCtxOutputPointerForSupplementScan(pSupporter, pMeterQueryInfo);
      }

      return TSDB_CODE_SUCCESS;
    }
  }

  assert(pMeterQueryInfo->lastResRows == 1);
  numOfResult = 1;
  pMeterQueryInfo->lastResRows = 0;

  if (IS_SUPPLEMENT_SCAN(pRuntimeEnv) && pMeterQueryInfo->reverseFillRes == 1) {
    assert(pMeterQueryInfo->numOfRes > 0 && pMeterQueryInfo->reverseIndex > 0 &&
           pMeterQueryInfo->reverseIndex <= pMeterQueryInfo->numOfRes);
    // backward one step from the previous position, the start position is (pMeterQueryInfo->numOfRows-1);
    pMeterQueryInfo->reverseIndex -= 1;
    setCtxOutputPointerForSupplementScan(pSupporter, pMeterQueryInfo);
  } else {
    int32_t    pageId = pMeterQueryInfo->pageList[pMeterQueryInfo->numOfPages - 1];
    tFilePage *pData = getFilePage(pSupporter, pageId);

    // in handling records occuring around '1970-01-01', the aligned start timestamp may be 0.
    TSKEY ts = *(TSKEY *)getOutputResPos(pRuntimeEnv, pData, pData->numOfElems, 0);

    SMeterObj *pMeterObj = pRuntimeEnv->pMeterObj;
    qTrace("QInfo:%p vid:%d sid:%d id:%s, save results, ts:%" PRId64 ", total:%d", GET_QINFO_ADDR(pQuery),
           pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId, ts, pMeterQueryInfo->numOfRes + 1);

    pData->numOfElems += numOfResult;
    pMeterQueryInfo->numOfRes += numOfResult;
    assert(pData->numOfElems <= pRuntimeEnv->numOfRowsPerPage);

    if (setOutputBufferForIntervalQuery(pSupporter, pMeterQueryInfo) != TSDB_CODE_SUCCESS) {
      return -1;
    }

    for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      resetResultInfo(&pMeterQueryInfo->resultInfo[i]);
    }

    validateResultBuf(pSupporter, pMeterQueryInfo);
    initCtxOutputBuf(pRuntimeEnv);
#if 0
    SSchema sc[TSDB_MAX_COLUMNS] = {0};
    sc[0].type = TSDB_DATA_TYPE_BIGINT;
    sc[0].bytes = 8;

    sc[1].type = TSDB_DATA_TYPE_BIGINT;
    sc[1].bytes = 8;

    UNUSED(sc);
    tColModel *cm = tColModelCreate(sc, pQuery->numOfOutputCols, pRuntimeEnv->numOfRowsPerPage);

//    if (outputPage->numOfElems + numOfResult >= pRuntimeEnv->numOfRowsPerPage)
        tColModelDisplay(cm, outputPage->data, outputPage->numOfElems, pRuntimeEnv->numOfRowsPerPage);
#endif
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t getSubsetNumber(SMeterQuerySupportObj *pSupporter) {
  SQuery *pQuery = pSupporter->runtimeEnv.pQuery;

  int32_t totalSubset = 0;
  if (isGroupbyNormalCol(pQuery->pGroupbyExpr) || (pQuery->nAggTimeInterval > 0 && pQuery->slidingTime > 0)) {
    totalSubset = numOfClosedSlidingWindow(&pSupporter->runtimeEnv.swindowResInfo);
  } else {
    totalSubset = pSupporter->pSidSet->numOfSubSet;
  }

  return totalSubset;
}

static int32_t doCopyFromGroupBuf(SMeterQuerySupportObj *pSupporter, SOutputRes *result, int32_t orderType) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  int32_t numOfResult = 0;
  int32_t startIdx = 0;
  int32_t forward = 1;

  dTrace("QInfo:%p start to copy data to dest buf", GET_QINFO_ADDR(pSupporter->runtimeEnv.pQuery));

  int32_t totalSubset = getSubsetNumber(pSupporter);

  if (orderType == TSQL_SO_ASC) {
    startIdx = pSupporter->subgroupIdx;
  } else {  // desc
    startIdx = totalSubset - pSupporter->subgroupIdx - 1;
    forward = -1;
  }

  for (int32_t i = startIdx; (i < totalSubset) && (i >= 0); i += forward) {
    if (result[i].numOfRows == 0) {
      pSupporter->offset = 0;
      pSupporter->subgroupIdx += 1;
      continue;
    }

    assert(result[i].numOfRows >= 0 && pSupporter->offset <= 1);

    tFilePage **srcBuf = result[i].result;

    int32_t numOfRowsToCopy = result[i].numOfRows - pSupporter->offset;
    int32_t oldOffset = pSupporter->offset;

    if (numOfRowsToCopy > pQuery->pointsToRead - numOfResult) {
      // current output space is not enough for the keep the data of this group
      numOfRowsToCopy = pQuery->pointsToRead - numOfResult;
      pSupporter->offset += numOfRowsToCopy;
    } else {
      pSupporter->offset = 0;
      pSupporter->subgroupIdx += 1;
    }

    for (int32_t j = 0; j < pQuery->numOfOutputCols; ++j) {
      int32_t elemSize = pRuntimeEnv->pCtx[j].outputBytes;
      char *  outputBuf = pQuery->sdata[j]->data + numOfResult * elemSize;

      memcpy(outputBuf, srcBuf[j]->data + oldOffset * elemSize, elemSize * numOfRowsToCopy);
    }

    numOfResult += numOfRowsToCopy;
    if (numOfResult == pQuery->pointsToRead) {
      break;
    }
  }

  dTrace("QInfo:%p done copy data to dst buf", GET_QINFO_ADDR(pSupporter->runtimeEnv.pQuery));

#ifdef _DEBUG_VIEW
  displayInterResult(pQuery->sdata, pQuery, numOfResult);
#endif
  return numOfResult;
}

/**
 * copyFromGroupBuf support copy data in ascending/descending order
 * @param pQInfo
 * @param result
 */
void copyFromGroupBuf(SQInfo *pQInfo, SOutputRes *result) {
  SQuery *               pQuery = &pQInfo->query;
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;

  int32_t orderType = (pQuery->pGroupbyExpr != NULL) ? pQuery->pGroupbyExpr->orderType : TSQL_SO_ASC;
  int32_t numOfResult = doCopyFromGroupBuf(pSupporter, result, orderType);

  pQuery->pointsRead += numOfResult;
  assert(pQuery->pointsRead <= pQuery->pointsToRead);
}

static void applyIntervalQueryOnBlock(SMeterQuerySupportObj *pSupporter, SMeterDataInfo *pMeterDataInfo,
                                      SBlockInfo *pBlockInfo, int32_t blockStatus, SField *pFields,
                                      __block_search_fn_t searchFn) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;
  SMeterQueryInfo * pMeterQueryInfo = pMeterDataInfo->pMeterQInfo;

  int64_t* pPrimaryKey = (int64_t*) pRuntimeEnv->primaryColBuffer->data;
  /*
   * for each block, we need to handle the previous query, since the determination of previous query being completed
   * or not is based on the start key of current block.
   */
  TSKEY key = getNextAccessedKeyInData(pQuery, pPrimaryKey, pBlockInfo, blockStatus);
  setIntervalQueryRange(pMeterDataInfo->pMeterQInfo, pSupporter, key);

  if (((pQuery->skey > pQuery->ekey) && QUERY_IS_ASC_QUERY(pQuery)) ||
      ((pQuery->skey < pQuery->ekey) && !QUERY_IS_ASC_QUERY(pQuery))) {
    return;
  }

  if (((pBlockInfo->keyLast < pQuery->ekey) && QUERY_IS_ASC_QUERY(pQuery)) ||
      ((pBlockInfo->keyFirst > pQuery->ekey) && !QUERY_IS_ASC_QUERY(pQuery))) {
    int32_t numOfRes = 0;
    /* current block is included in this interval */
    int32_t steps = applyFunctionsOnBlock(pRuntimeEnv, pBlockInfo, pPrimaryKey, pFields, searchFn, &numOfRes);
    assert(numOfRes <= 1 && numOfRes >= 0 && steps > 0);

    if (pMeterQueryInfo->lastResRows == 0) {
      pMeterQueryInfo->lastResRows = numOfRes;
    } else {
      assert(pMeterQueryInfo->lastResRows == 1);
    }

    saveIntervalQueryRange(pRuntimeEnv, pMeterQueryInfo);
  } else {
    doApplyIntervalQueryOnBlock(pSupporter, pMeterQueryInfo, pBlockInfo, pPrimaryKey, pFields, searchFn);
  }
}

// we need to split the refstatsult into different packages.
int32_t vnodeGetResultSize(void *thandle, int32_t *numOfRows) {
  SQInfo *pQInfo = (SQInfo *)thandle;
  SQuery *pQuery = &pQInfo->query;

  /*
   * get the file size and set the numOfRows to be the file size, since for tsComp query,
   * the returned row size is equalled to 1
   *
   * TODO handle the case that the file is too large to send back one time
   */
  if (pQInfo->pMeterQuerySupporter != NULL && isTSCompQuery(pQuery) && (*numOfRows) > 0) {
    struct stat fstat;
    if (stat(pQuery->sdata[0]->data, &fstat) == 0) {
      *numOfRows = fstat.st_size;
      return fstat.st_size;
    } else {
      dError("QInfo:%p failed to get file info, path:%s, reason:%s", pQInfo, pQuery->sdata[0]->data, strerror(errno));
      return 0;
    }
  } else {
    return pQInfo->query.rowSize * (*numOfRows);
  }
}

int64_t vnodeGetOffsetVal(void *thandle) {
  SQInfo *pQInfo = (SQInfo *)thandle;
  return pQInfo->query.limit.offset;
}

bool vnodeHasRemainResults(void *handle) {
  SQInfo *               pQInfo = (SQInfo *)handle;
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;

  if (pSupporter == NULL || pQInfo->query.interpoType == TSDB_INTERPO_NONE) {
    return false;
  }

  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *          pQuery = pRuntimeEnv->pQuery;

  SInterpolationInfo *pInterpoInfo = &pRuntimeEnv->interpoInfo;
  if (pQuery->limit.limit > 0 && pQInfo->pointsRead >= pQuery->limit.limit) {
    return false;
  }

  int32_t remain = taosNumOfRemainPoints(pInterpoInfo);
  if (remain > 0) {
    return true;
  } else {
    if (pRuntimeEnv->pInterpoBuf == NULL) {
      return false;
    }

    // query has completed
    if (Q_STATUS_EQUAL(pQuery->over, QUERY_COMPLETED | QUERY_NO_DATA_TO_CHECK)) {
      TSKEY   ekey = taosGetRevisedEndKey(pSupporter->rawEKey, pQuery->order.order, pQuery->nAggTimeInterval,
                                        pQuery->intervalTimeUnit, pQuery->precision);
      int32_t numOfTotal = taosGetNumOfResultWithInterpo(pInterpoInfo, (TSKEY *)pRuntimeEnv->pInterpoBuf[0]->data,
                                                         remain, pQuery->nAggTimeInterval, ekey, pQuery->pointsToRead);
      return numOfTotal > 0;
    }

    return false;
  }
}

static int32_t resultInterpolate(SQInfo *pQInfo, tFilePage **data, tFilePage **pDataSrc, int32_t numOfRows,
                                 int32_t outputRows) {
  SQuery *          pQuery = &pQInfo->query;
  SQueryRuntimeEnv *pRuntimeEnv = &pQInfo->pMeterQuerySupporter->runtimeEnv;

  assert(pRuntimeEnv->pCtx[0].outputBytes == TSDB_KEYSIZE);

  // build support structure for performing interpolation
  SSchema *pSchema = calloc(1, sizeof(SSchema) * pQuery->numOfOutputCols);
  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    pSchema[i].bytes = pRuntimeEnv->pCtx[i].outputBytes;
    pSchema[i].type = pQuery->pSelectExpr[i].resType;
  }

  tColModel *pModel = tColModelCreate(pSchema, pQuery->numOfOutputCols, pQuery->pointsToRead);

  char *  srcData[TSDB_MAX_COLUMNS] = {0};
  int32_t functions[TSDB_MAX_COLUMNS] = {0};

  for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
    srcData[i] = pDataSrc[i]->data;
    functions[i] = pQuery->pSelectExpr[i].pBase.functionId;
  }

  int32_t numOfRes = taosDoInterpoResult(&pRuntimeEnv->interpoInfo, pQuery->interpoType, data, numOfRows, outputRows,
                                         pQuery->nAggTimeInterval, (int64_t *)pDataSrc[0]->data, pModel, srcData,
                                         pQuery->defaultVal, functions, pRuntimeEnv->pMeterObj->pointsPerFileBlock);

  tColModelDestroy(pModel);
  free(pSchema);

  return numOfRes;
}

static void doCopyQueryResultToMsg(SQInfo *pQInfo, int32_t numOfRows, char *data) {
  SMeterObj *pObj = pQInfo->pObj;
  SQuery *   pQuery = &pQInfo->query;

  int tnumOfRows = vnodeList[pObj->vnode].cfg.rowsInFileBlock;

  // for metric query, bufIndex always be 0.
  for (int32_t col = 0; col < pQuery->numOfOutputCols; ++col) {  // pQInfo->bufIndex == 0
    int32_t bytes = pQuery->pSelectExpr[col].resBytes;

    memmove(data, pQuery->sdata[col]->data + bytes * tnumOfRows * pQInfo->bufIndex, bytes * numOfRows);
    data += bytes * numOfRows;
  }
}

/**
 * Copy the result data/file to output message buffer.
 * If the result is in file format, read file from disk and copy to output buffer, compression is not involved since
 * data in file is already compressed.
 * In case of other result in buffer, compress the result before copy once the tsComressMsg is set.
 *
 * @param handle
 * @param data
 * @param numOfRows the number of rows that are not returned in current retrieve
 * @return
 */
int32_t vnodeCopyQueryResultToMsg(void *handle, char *data, int32_t numOfRows) {
  SQInfo *pQInfo = (SQInfo *)handle;
  SQuery *pQuery = &pQInfo->query;

  assert(pQuery->pSelectExpr != NULL && pQuery->numOfOutputCols > 0);

  // load data from file to msg buffer
  if (isTSCompQuery(pQuery)) {
    int32_t fd = open(pQuery->sdata[0]->data, O_RDONLY, 0666);

    // make sure file exist
    if (FD_VALID(fd)) {
      size_t s = lseek(fd, 0, SEEK_END);
      dTrace("QInfo:%p ts comp data return, file:%s, size:%zu", pQInfo, pQuery->sdata[0]->data, s);

      lseek(fd, 0, SEEK_SET);
      read(fd, data, s);
      close(fd);

      unlink(pQuery->sdata[0]->data);
    } else {
      dError("QInfo:%p failed to open tmp file to send ts-comp data to client, path:%s, reason:%s", pQInfo,
             pQuery->sdata[0]->data, strerror(errno));
    }
  } else {
    doCopyQueryResultToMsg(pQInfo, numOfRows, data);
  }

  return numOfRows;
}

int32_t vnodeQueryResultInterpolate(SQInfo *pQInfo, tFilePage **pDst, tFilePage **pDataSrc, int32_t numOfRows,
                                    int32_t *numOfInterpo) {
  SMeterQuerySupportObj *pSupporter = pQInfo->pMeterQuerySupporter;
  SQueryRuntimeEnv *     pRuntimeEnv = &pSupporter->runtimeEnv;
  SQuery *               pQuery = pRuntimeEnv->pQuery;

  while (1) {
    numOfRows = taosNumOfRemainPoints(&pRuntimeEnv->interpoInfo);

    TSKEY   ekey = taosGetRevisedEndKey(pSupporter->rawEKey, pQuery->order.order, pQuery->nAggTimeInterval,
                                      pQuery->intervalTimeUnit, pQuery->precision);
    int32_t numOfFinalRows =
        taosGetNumOfResultWithInterpo(&pRuntimeEnv->interpoInfo, (TSKEY *)pDataSrc[0]->data, numOfRows,
                                      pQuery->nAggTimeInterval, ekey, pQuery->pointsToRead);

    int32_t ret = resultInterpolate(pQInfo, pDst, pDataSrc, numOfRows, numOfFinalRows);
    assert(ret == numOfFinalRows);

    /* reached the start position of according to offset value, return immediately */
    if (pQuery->limit.offset == 0) {
      return ret;
    }

    if (pQuery->limit.offset < ret) {
      ret -= pQuery->limit.offset;
      // todo !!!!there exactly number of interpo is not valid.
      // todo refactor move to the beginning of buffer
      //      if (QUERY_IS_ASC_QUERY(pQuery)) {
      for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
        memmove(pDst[i]->data, pDst[i]->data + pQuery->pSelectExpr[i].resBytes * pQuery->limit.offset,
                ret * pQuery->pSelectExpr[i].resBytes);
      }
      //      } else {
      //        for (int32_t i = 0; i < pQuery->numOfOutputCols; ++i) {
      //          memmove(pDst[i]->data + (pQuery->pointsToRead - ret) * pQuery->pSelectExpr[i].resBytes,
      //                  pDst[i]->data + (pQuery->pointsToRead - ret - pQuery->limit.offset) *
      //                  pQuery->pSelectExpr[i].resBytes, ret * pQuery->pSelectExpr[i].resBytes);
      //        }
      //      }
      pQuery->limit.offset = 0;
      return ret;
    } else {
      pQuery->limit.offset -= ret;
      ret = 0;
    }

    if (!vnodeHasRemainResults(pQInfo)) {
      return ret;
    }
  }
}

void vnodePrintQueryStatistics(SMeterQuerySupportObj *pSupporter) {
  SQueryRuntimeEnv *pRuntimeEnv = &pSupporter->runtimeEnv;

  SQuery *pQuery = pRuntimeEnv->pQuery;
  SQInfo *pQInfo = (SQInfo *)GET_QINFO_ADDR(pQuery);

  SQueryCostSummary *pSummary = &pRuntimeEnv->summary;
  pSummary->tmpBufferInDisk = pSupporter->bufSize;

  dTrace("QInfo:%p statis: comp blocks:%d, size:%d Bytes, elapsed time:%.2f ms", pQInfo, pSummary->readCompInfo,
         pSummary->totalCompInfoSize, pSummary->loadCompInfoUs / 1000.0);

  dTrace("QInfo:%p statis: field info: %d, size:%d Bytes, avg size:%.2f Bytes, elapsed time:%.2f ms", pQInfo,
         pSummary->readField, pSummary->totalFieldSize, (double)pSummary->totalFieldSize / pSummary->readField,
         pSummary->loadFieldUs / 1000.0);

  dTrace(
      "QInfo:%p statis: file blocks:%d, size:%d Bytes, elapsed time:%.2f ms, skipped:%d, in-memory gen null:%d Bytes",
      pQInfo, pSummary->readDiskBlocks, pSummary->totalBlockSize, pSummary->loadBlocksUs / 1000.0,
      pSummary->skippedFileBlocks, pSummary->totalGenData);

  dTrace("QInfo:%p statis: cache blocks:%d", pQInfo, pSummary->blocksInCache, 0);
  dTrace("QInfo:%p statis: temp file:%d Bytes", pQInfo, pSummary->tmpBufferInDisk);

  dTrace("QInfo:%p statis: file:%d, table:%d", pQInfo, pSummary->numOfFiles, pSummary->numOfTables);
  dTrace("QInfo:%p statis: seek ops:%d", pQInfo, pSummary->numOfSeek);

  double total = pSummary->fileTimeUs + pSummary->cacheTimeUs;
  double io = pSummary->loadCompInfoUs + pSummary->loadBlocksUs + pSummary->loadFieldUs;
  //    assert(io <= pSummary->fileTimeUs);

  // todo add the intermediate result save cost!!
  double computing = total - io;

  dTrace(
      "QInfo:%p statis: total elapsed time:%.2f ms, file:%.2f ms(%.2f%), cache:%.2f ms(%.2f%). io:%.2f ms(%.2f%),"
      "comput:%.2fms(%.2f%)",
      pQInfo, total / 1000.0, pSummary->fileTimeUs / 1000.0, pSummary->fileTimeUs * 100 / total,
      pSummary->cacheTimeUs / 1000.0, pSummary->cacheTimeUs * 100 / total, io / 1000.0, io * 100 / total,
      computing / 1000.0, computing * 100 / total);
}
