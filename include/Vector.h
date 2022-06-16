#ifndef VECTOR_H
#define VECTOR_H

//---------------------------------------------------------------------
//  ____ 
// |  _ \    This file is part of  PC2L:  A Parallel & Cloud Computing 
// | |_) |   Library <http://www.pc2lab.cec.miamioh.edu/pc2l>. PC2L is 
// |  __/    free software: you can  redistribute it and/or  modify it
// |_|       under the terms of the GNU  General Public License  (GPL)
//           as published  by  the   Free  Software Foundation, either
//           version 3 (GPL v3), or  (at your option) a later version.
//    
//   ____    PC2L  is distributed in the hope that it will  be useful,
//  / ___|   but   WITHOUT  ANY  WARRANTY;  without  even  the IMPLIED
// | |       WARRANTY of  MERCHANTABILITY  or FITNESS FOR A PARTICULAR
// | |___    PURPOSE.
//  \____| 
//            Miami University and  the PC2Lab development team make no
//            representations  or  warranties  about the suitability of
//  ____      the software,  either  express  or implied, including but
// |___ \     not limited to the implied warranties of merchantability,
//   __) |    fitness  for a  particular  purpose, or non-infringement.
//  / __/     Miami  University and  its affiliates shall not be liable
// |_____|    for any damages  suffered by the  licensee as a result of
//            using, modifying,  or distributing  this software  or its
//            derivatives.
//
//  _         By using or  copying  this  Software,  Licensee  agree to
// | |        abide  by the intellectual  property laws,  and all other
// | |        applicable  laws of  the U.S.,  and the terms of the  GNU
// | |___     General  Public  License  (version 3).  You  should  have
// |_____|    received a  copy of the  GNU General Public License along
//            with MUSE.  If not,  you may  download  copies  of GPL V3
//            from <http://www.gnu.org/licenses/>.
//
// --------------------------------------------------------------------
// Authors:   JD Rudie               rudiejd@miamioh.edu
//---------------------------------------------------------------------
/**
 * @file Vector.h
 * @brief Definition of Vector
 * @author JD Rudie
 * @version 0.1
 * @date 2021-08-30
 * 
 */

#include <iterator>
#include <unordered_map>
#include "Worker.h"
#include "CacheManager.h"
#include "System.h"
#include "MPIHelper.h"
#include "Message.h"


// namespace pc2l {
BEGIN_NAMESPACE(pc2l);


/**
 * A distributed vector that runs across multiple machines
 * utilizing message passing through MPI. This initial  
 * implementation does not include any caching.
 */
template <typename T>
class Vector {
public:

    struct Iterator : public std::iterator<
                                std::input_iterator_tag,
                                T,
                                T,
                                const T*,
                                T> {
        friend class Vector<T>;
    public:
        // Return value of data at index i
        T operator*() const { return vec.at(i); };

        // Maybe implement bounds check here
        Iterator& operator++() { i++; return *this; }
        Iterator& operator--() { i--; return *this; }

        bool operator==(const Iterator& rhs) const {
            return &rhs.vec == &vec && rhs.i == i;
        }
        bool operator!=(const Iterator& rhs) const {
            return !(*this == rhs);
        }

    private:
        Iterator(const Vector<T>& vec, const int end = 0) :
            vec(vec), i(end) {}
        const Vector<T>& vec;
        size_t i = 0;
    };


    // Iterator methods
    Vector<T>::Iterator begin() const {return Iterator(*this); }
    Vector<T>::Iterator end() const {return Iterator(*this, size()); }

    /**
     * The default constructor.  Currently, the constructor calls the
     * workhorse
     */
    Vector() : blockSize(System::get().getBlockSize()), siz(0), dsTag(System::get().dsCount++){ }

    /**
     * The destructor.
     */
    virtual ~Vector() = default;

    // unique identifier for this data structure
    size_t dsTag;

    // the size (in bytes) of each block in vector. Potentially offer heterogeneous
    // block sizes on different data structures later, but for now it is uniform
    unsigned long long blockSize;

    // The number of elements currently in the vector
    unsigned long long siz;

    // block tag of last retrieved block
   mutable size_t prevBlockTag;

    // reference to message containing last retrieved block
    mutable MessagePtr prevMsg;


    /**
     * Returns size (in values, not blocks) of vector
     * @return size (in values) of vector
     */
    unsigned long long size() const {
        return siz;
    }

    /**
     * Erase all values from vector
     */
    void clear() {
        for (unsigned long long i = 0; i < siz; i++) {
            erase(i);
        }
    }

    /**
     * Swap value at index \p i with value at index \j
     * @param i first index of swap
     * @param j second index of swap
     */
    void swap(size_t i, size_t j) {
        auto oldI = at(i);
        replace(i, at(j));
        replace(j, oldI);
    }

    /**
     * Erase the value at \p index
     * @param index the index of the value to be erased
     */
    void erase(unsigned long long index) {
        // move all of the blocks to the right of the index left by one
        for (unsigned long long i = index; i < size() - 1; i++) { swap(i, i + 1); }
        siz--;
        //TODO: maybe some check to see if it is successfully deleted?
    }

    /**
     * Returns value at \p index. Note that this only returns a value, not a reference
     * (for now)
     * @param index
     * @return The value at \p index
     */
    T at(unsigned long long index) const {
        static int TypeSize = 2, BlockShiftBits = 5, IndexMask = 31;
        // instead of div, prefer bitwise operations eventually
        // but this will require moving to powers of 2 only

        // For now we are hardcoding some of this to just test to see
        // what is the performance improvement that we may be able to
        // achieve with bitwise operations.
        
        // const auto ret = std::lldiv(index*sizeof(T), blockSize);
        // const size_t blockTag = ret.quot;
        // unsigned long long inBlockIdx = ret.rem;

        // @insert: index = 99, blockTag = 12, inBlockIdx = 12
        // @at: index = 99, blockTag = 49, inBlockIdx = 4
            
        const size_t offset   = index << TypeSize;
        const size_t blockTag = (offset >> BlockShiftBits),
            inBlockIdx = (offset & IndexMask);
        // std::cout << "@at: index = " << index << ", blockTag = " << blockTag
        //           << ", inBlockIdx = " << inBlockIdx << std::endl;

        if (blockTag == prevBlockTag) {
            // if the CacheManager's cache contains this block, just get it
            // get array of concatenated T-serializations
            char* payload = prevMsg->getPayload();
            return *reinterpret_cast<T*>(payload + inBlockIdx);
        }
        CacheManager& cm  = System::get().cacheManager();
        MessagePtr msg = cm.getBlockFallbackRemote(dsTag, blockTag);
        prevBlockTag = blockTag;
        prevMsg = msg;
        // if the CacheManager's cache contains this block, just get it
        // get array of concatenated T-serializations
        char* payload = msg->getPayload();
        // offset into this array and extract correct portion
        return *reinterpret_cast<T*>(payload + inBlockIdx);
    }

    /**
     * Insert \p value at vector index \p index.
     * @param index index where insert should occur
     * @param value value to be inserted
     */
    void insert(unsigned long long index, T value) {
        CacheManager &cm = System::get().cacheManager();
        if (index < size()) {
            // all other values shifted right one index (size incremented here)
            // we have to do this BEFORE the (potentially evicted) message with
            // this value in it is retrieved
            insert(size(), at(size() - 1));
            for (auto i = size() - 2; i > index; i--) {
                replace(i, at(i - 1));
//                for(size_t j = 0; j < size(); j++) {
//                    if (MPI_GET_RANK() == 0)
//                        std::cout << at(j) << std::endl;
//                }
            }
        }
        size_t blockTag = index * sizeof(T) / blockSize;
        MessagePtr msg;
        if (prevBlockTag == blockTag) {
            msg = prevMsg;
        } else if (index < size()) {
            // fetch from cache manager or remote CW
            msg = cm.getBlockFallbackRemote(dsTag, blockTag);
        } else {
            // if insert at end, make new block+
            msg = Message::create(blockSize, Message::STORE_BLOCK, 0, dsTag, blockTag);
        }
        char *block = msg->getPayload();
        // offset into the block array of serializations and insert val
        unsigned long long inBlockIdx = ((index * sizeof(T)) % blockSize);
        // std::cout << "@insert: index = " << index << ", blockTag = "
        //         << blockTag << ", inBlockIdx = " << inBlockIdx << std::endl;
        char *serialized = reinterpret_cast<char *>(&value);
        std::copy(&serialized[0], &serialized[sizeof(T)], &block[inBlockIdx]);
        // then put the object at retrieved index into cache
        prevMsg = msg;
        prevBlockTag = blockTag;
        cm.storeCacheBlock(msg);
        // if it's an insert at the end, we haven't yet incremented size. otherwise we have
        if (index == size()) siz++;
    }

    /**
     * Alias for inserting at "back" (largest index) of vector
     * @param value value to be inserted
     */
    void push_back(T value) {
       insert(size(), value);
    }

    /**
     * Replace value at \p index with \p value
     * @param index
     * @param value
     */
    void replace(unsigned long long index, T value) {
        // instead of div, prefer bitwise operations eventually
        // but this will require moving to powers of 2 only
        const auto ret = std::lldiv(index*sizeof(T), blockSize);
        const size_t blockTag = ret.quot;
        unsigned long long inBlockIdx = ret.rem;
        MessagePtr msg;
        CacheManager& cm = System::get().cacheManager();
        if (blockTag == prevBlockTag) {
            msg = prevMsg;
        } else {
            msg = cm.getBlockFallbackRemote(dsTag, blockTag);
        }
        char* block = msg->getPayload();
        // fill the buffer with new datum at correct in-blok offset
        char* serialized = reinterpret_cast<char*>(&value);
        std::copy(&serialized[0], &serialized[sizeof(T)], &block[inBlockIdx]);
        prevMsg = msg;
        prevBlockTag = blockTag;
        cm.storeCacheBlock(msg);
    }



    /**
     * Sort vector in ascending order using mergesort
     */
    void sort() {
        mergesort(0, size() - 1);
    }
private:
    void merge(int low, int mid, int high) {
        auto secondLow = mid + 1;

        // if merge already sorted
        if (at(mid) <= at(secondLow)) {
           return;
        }

        while (low <= mid && secondLow <= high) {
            // first element is in right place
            if (at(low) <= at(secondLow)) {
                low++;
            } else {
                auto val = at(secondLow);
                auto idx = secondLow;

                // Shift all shit between low and 2nd low right by one
               while (idx != low) {
                   replace(idx, at(idx -1));
                   idx--;
               }
               replace(low, val);

               // update indices
               low++;
               mid++;
               secondLow++;
            }
        }
    }

// Iteratively sort subarray `A[low…high]` using a temporary array
    void mergesort(unsigned long long low, unsigned long long high) {
        if (low < high) {

            // avoid overflow for large low/high indices
            auto mid = low + (high - low)  / 2;

            mergesort(low, mid);
            mergesort(mid + 1, high);

            merge(low, mid, high);

        }
    }
};

END_NAMESPACE(pc2l);
// }   // end namespace pc2l

#endif

