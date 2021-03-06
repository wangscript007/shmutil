/*
 * shm_mrsw_hashtable.cc
 *
 *      Descript: mutli-reader-single-writer hashtable implementation in shared memory.
 *      Created on: 2017.5.9
 *      Author: ZengHui Bao (bao_z_h@163.com)
 */

#include <errno.h>
#include "common.h"
#include "shm_mrsw_hashtable.h"

MrswHashtable::MrswHashtable()
{
    table_ = NULL;
    isLruEliminate_ = true;
    isInit_ = false;
}

MrswHashtable::~MrswHashtable()
{
    mutex_.lock();
    if (table_)
    {
        shmdt((const void*)table_);
        table_ = NULL;
        isInit_ = false;
    }
    mutex_.unlock();
}

bool MrswHashtable::isInit()
{
    return isInit_;
}

ErrorCode MrswHashtable::init(key_t shmkey, int maxSlotNum, bool isLruEliminate)
{
    int shmId = -1;
    size_t memSize = 0;
    void* pShm = NULL;

    memSize = qhasharr_calculate_memsize(maxSlotNum);
    shmId = shmget(shmkey, memSize, IPC_CREAT | IPC_EXCL | 0666);
    if (-1 == shmId)
    {
        if (EEXIST == errno)
        {
            return getShm(shmkey, isLruEliminate);
        }

        fprintf(stderr, "[init] Failed to create share memory, key:%d, errno:%d\n", shmkey, errno);
        return kErrShmGet;
    }

    pShm = shmat(shmId, NULL, 0);
    if ((void*)-1 == pShm)
    {
        fprintf(stderr, "[init] Failed to shmat, key:%d, errno:%d\n", shmkey, errno);
        return kErrShmAt;
    }

    table_ = qhasharr(pShm, memSize);
    if (NULL == table_)
    {
        fprintf(stderr, "[init] Failed to init shm! shmId:%d, errno:%d\n", shmId, errno);
        return kErrShmInit;
    }

    isLruEliminate_ = isLruEliminate;
    isInit_ = true;
    return kOk;
}

ErrorCode MrswHashtable::getShm(key_t shmkey, bool isLruEliminate)
{
    int shmId = -1;

    shmId = shmget(shmkey, 0, 0644);
    if (-1 == shmId) return kErrShmGet;

    table_ = (qhasharr_t*)shmat(shmId, NULL, 0);
    if ((void*)-1 == (void*)table_)
    {
        table_ = NULL;
        return kErrShmAt;
    }

    isLruEliminate_ = isLruEliminate;
    if (isLruEliminate_)
    {
        ErrorCode ret = initLruMem(table_);
        if (ret == kOk)
        {
            isInit_ = true;
        }
        return ret;
    }
    else
    {
        isInit_ = true;
        return kOk;
    }
}

ErrorCode MrswHashtable::initLruMem(qhasharr_t* tbl)
{
    int maxSlots = 0, usedSlots = 0;
    qhasharr_size(tbl, &maxSlots, &usedSlots);

    std::string tblkey, tblval;
    for (int idx = 0; idx < maxSlots;)
    {
        int ret = getNextKVPair(tbl, tblkey, tblval, idx);
        if (ret == kOk)
        {
            if (!tblkey.empty())
            {
                lru_.visitKey(tblkey);
            }
        }
        else if (ret == kErrTraverseTableEnd)
        { break;}
        else
        {
            fprintf(stderr, "[initLruMem] Failed to get next item in shmtbl. errCode:%d\n", ret);
            return kErrLruInit;
        }
    }
    return kOk;
}

ErrorCode MrswHashtable::getNextKVPair(qhasharr_t *tbl, std::string &tblkey, std::string &tblval, int &idx)
{
    if (NULL == tbl) return kErrInvalidParams;

    qnobj_t obj;
    memset(&obj, 0, sizeof(qnobj_t));

    mutex_.lock();
    bool status = qhasharr_getnext(tbl, &obj, &idx);
    mutex_.unlock();
    if (!status)
    {
        idx++;
        if (ENOENT == errno && idx >= (tbl->maxslots+1)) return kErrTraverseTableEnd;

        fprintf(stderr, "[getNextKVPair] qhasharr_getnext failed! idx:%d, errno:%d\n", idx-1, errno);
        return kErrNotFound;
    }

    tblval.assign((char*)obj.data, obj.data_size);
    ErrorCode ret = verifyShmValue(tblval);
    if (ret != kOk)
    {
        free(obj.name);
        free(obj.data);
        return ret;
    }

    tblkey.assign(obj.name, obj.name_size);
    free(obj.name);
    free(obj.data);

    return kOk;
}

//校验value是否合法，如果合法，则把去掉检验码的字符串 赋给tblval
ErrorCode MrswHashtable::verifyShmValue(std::string &tblval)
{
    size_t originalValSize = tblval.size();
    const char *valStr = NULL, *verifyStr = NULL;
    char valMD5Str[MD5_STR_LEN] = {0};

    uint32_t valLen = decodeValueLen(tblval.data());
    valStr = tblval.data() + sizeof(uint32_t);
    verifyStr = valStr + valLen;

    // verify MD5 code
    if (valLen > NEED_MD5_VALUE_LEN)
    {
        if (originalValSize < sizeof(uint32_t) + valLen + MD5_STR_LEN)
        {
            return kErrDataMess;
        }

        qhashmd5(valStr, valLen, valMD5Str);
        if (memcmp(valMD5Str, verifyStr, MD5_STR_LEN) == 0)
        {
            tblval.assign(valStr, valLen);  //把去掉检验码的字符串 赋给 tblval
            return kOk;
        }
    }
    else   // verify original value
    {
        if (originalValSize < sizeof(uint32_t) + valLen * 2)
        {
            return kErrDataMess;
        }
        if (memcmp(valStr, verifyStr, valLen) == 0)
        {
            tblval.assign(valStr, valLen);
            return kOk;
        }
    }
    return kErrDataMess;
}

ErrorCode MrswHashtable::getValue(const std::string &key, std::string &val)
{
    if (!isInit() || key.empty()) return kErrInvalidParams;

    char *tmpVal = NULL;
    size_t tmpValLen = 0;

    mutex_.lock();
    tmpVal = (char*)qhasharr_get(table_, key.data(), key.size(), &tmpValLen);
    mutex_.unlock();
    if (NULL == tmpVal) return kErrNotFound;

    val.assign(tmpVal, tmpValLen);
    free(tmpVal);
    tmpVal = NULL;

    return verifyShmValue(val);
}

ErrorCode MrswHashtable::setValue(const std::string &key, const std::string &val)
{
    if (!isInit() || key.empty()) return kErrInvalidParams;

    std::string valInTbl;
    ErrorCode ret = getValue(key, valInTbl);
    if (ret == kOk && 0 == val.compare(valInTbl))
        return kErrSetSameValue;

    std::string tmpVal = encodeValueStr(val);
    return __setValue(table_, key, tmpVal);
}

std::string MrswHashtable::encodeValueStr(const std::string &val)
{
 /*        __________
 *       |          |
 *       v          |
 *  | value len | value | verification code |
 */
    std::string tmpVal;
    char md5Val[MD5_STR_LEN] = {0};

    uint32_t val_size = val.size();
    char buf[sizeof(uint32_t)] = {0};
    encodeValueLen(buf, val_size);
    tmpVal.assign(buf, sizeof(uint32_t));
    tmpVal.append(val);

    // Use MD5 as verification code
    if (val_size > NEED_MD5_VALUE_LEN)
    {
        qhashmd5(val.data(), val_size, md5Val);
        tmpVal.append(md5Val, MD5_STR_LEN);
    }
    else  // Use original value as verification code
    {
        tmpVal += val;
    }
    return tmpVal;
}

ErrorCode MrswHashtable::__setValue(qhasharr_t *tbl, const std::string &key, const std::string &val)
{
    if (key.empty()) return kErrInvalidParams;
    mutex_.lock();
    bool ret = qhasharr_put(tbl, key.data(), key.size(), val.data(), val.size());
    mutex_.unlock();

    if (!ret && errno == ENOBUFS)
    {
        //如果由于 共享内存的空间不够 导致插入失败，则 删除过旧的key/value，再尝试插入.
        if (isLruEliminate_)
        {
            while (!ret && errno == ENOBUFS)
            {
                std::string removeKey = lru_.getRemoveKey();
                ErrorCode ecRet = deleteKey(removeKey);
                if (ecRet == kOk) {
                    lru_.removeKey();
                }
                else
                {
                    fprintf(stderr, "[__setValue] remove key from shared memory failed!");
                    return ecRet;
                }
                mutex_.lock();
                ret = qhasharr_put(tbl, key.data(), key.size(), val.data(), val.size());
                mutex_.unlock();
            }
        }
        else
        {
            return kErrNotEnoughSpace;
        }
    }

    if (ret)
    {
        lru_.visitKey(key);
    }
    return ret ? kOk : kErrStoreKVFailed;
}

ErrorCode MrswHashtable::isExist(const std::string &key, bool &exist)
{
    if (!isInit() || key.empty()) return kErrInvalidParams;

    mutex_.lock();
    exist = qhasharr_exist(table_, key.data(), key.size());
    mutex_.unlock();
    return kOk;
}

ErrorCode MrswHashtable::deleteKey(const std::string &key)
{
    if (!isInit() || key.empty()) return kErrInvalidParams;

    mutex_.lock();
    bool ret = qhasharr_remove(table_, key.data(), key.size());
    mutex_.unlock();

    if (!ret)
    {
        return (ENOENT == errno) ? kOk: kErrOther;
    }
    else
    {
        return kOk;
    }
}

ErrorCode MrswHashtable::getNext(std::string &key, std::string &val, int &index)
{
    return getNextKVPair(table_, key, val, index);
}

ErrorCode MrswHashtable::getStats(int &maxSlots, int &usedSlots, int &usedKeys)
{
    if (!isInit()) return kErrShmNotInit;
    qhasharr_size(table_, &maxSlots, &usedSlots);
    usedKeys = table_->num;
    return kOk;
}

ErrorCode MrswHashtable::clearTable()
{
    if (!isInit()) return kErrShmNotInit;
    mutex_.lock();
    qhasharr_clear(table_);
    mutex_.unlock();
    return kOk;
}
