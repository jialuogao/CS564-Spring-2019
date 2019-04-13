/**
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University
 * of Wisconsin-Madison.
 */

#include "btree.h"
#include "exceptions/bad_index_info_exception.h"
#include "exceptions/bad_opcodes_exception.h"
#include "exceptions/bad_scanrange_exception.h"
#include "exceptions/end_of_file_exception.h"
#include "exceptions/file_not_found_exception.h"
#include "exceptions/index_scan_completed_exception.h"
#include "exceptions/no_such_key_found_exception.h"
#include "exceptions/scan_not_initialized_exception.h"
#include "filescan.h"

using namespace std;

namespace badgerdb {

/** Generic Helper**/

bool isLeaf(Page *page) {
  return *((int *) page) == -1;
}

bool isNonLeafNodeFull(NonLeafNodeInt *node) {
  // assumption: pageID is zero => unoccupied
  return node->pageNoArray[INTARRAYNONLEAFSIZE] != 0;
}

bool isLeafNodeFull(LeafNodeInt *node) {
  // assumption: RecordID is zero => unoccupied
  return node->ridArray[INTARRAYLEAFSIZE - 1] != RecordId{};
}

void setNode(Page *page, void *node) {
  memcpy(page, node, Page::SIZE);
}

NonLeafNodeInt *getNonLeafNodeFromPage(Page *page) {
  return (NonLeafNodeInt *) page;
}

LeafNodeInt *getLeafNodeFromPage(Page *page) {
  return (LeafNodeInt *) page;
}

const string getIndexName(const string &relationName,
                          const int attrByteOffset) {
  ostringstream idxStr;
  idxStr << relationName << ',' << attrByteOffset;
  return idxStr.str();
}

int findArrayIndex(const int *arr, int len, int key,
                   bool includeCurrentKey = true) {
  if (includeCurrentKey) {
    for (int i = 0; i < len; i++)
      if (arr[i] >= key) return i;
  } else {
    for (int i = 0; i < len; i++)
      if (arr[i] > key) return i;
  }
  return -1;
}

int getLeafLen(LeafNodeInt *node) {
  RecordId emptyRid{};

  for (int i = 0; i < INTARRAYLEAFSIZE; i++)
    if (node->ridArray[i] == emptyRid) return i;

  return INTARRAYLEAFSIZE;
}

int getNonLeafLen(NonLeafNodeInt *node) {
  for (int i = 1; i <= INTARRAYNONLEAFSIZE; i++)
    if (node->pageNoArray[i] == 0) return i;

  return INTARRAYNONLEAFSIZE + 1;
}

/**
 * return the largest index if not found
 */
int findIndexNonLeaf(NonLeafNodeInt *node, int key) {
  int len = getNonLeafLen(node);
  int result = findArrayIndex(node->keyArray, len - 1, key);
  return result == -1 ? len - 1 : result;
}

/**
 * return next insertion index if not found
 */
int findInsertionIndexLeaf(LeafNodeInt *node, int key) {
  int len = getLeafLen(node);
  int result = findArrayIndex(node->keyArray, len, key);
  return result == -1 ? len : result;
}

/**
 * return -1 if not found
 */
int findScanIndexLeaf(LeafNodeInt *node, int key, bool includeCurrentKey) {
  return findArrayIndex(node->keyArray, getLeafLen(node), key, includeCurrentKey);
}

NonLeafNodeInt *splitNonLeafNode(NonLeafNodeInt *node, int index,
                                 bool moveKeyUp) {
  NonLeafNodeInt *newNode = new NonLeafNodeInt();
  size_t rightsize = INTARRAYNONLEAFSIZE - index;
  void *keyptr = &(node->keyArray[index]);
  // set keyArray
  if (moveKeyUp) {
    memcpy(&newNode->keyArray, keyptr, rightsize * sizeof(int));
    memset(keyptr, 0, rightsize * sizeof(int));
  } else {
    void *keyptr2 = &(node->keyArray[index + 1]);
    memcpy(&newNode->keyArray, keyptr2, (rightsize - 1) * sizeof(int));
    memset(keyptr, 0, rightsize * sizeof(int));
  }
  // set pgNoArray
  void *pgnptr = &(node->pageNoArray[index + 1]);
  memcpy(&newNode->pageNoArray, pgnptr, rightsize * sizeof(PageId));
  memset(pgnptr, 0, rightsize * sizeof(PageId));
  return newNode;
}

LeafNodeInt *splitLeafNode(LeafNodeInt *node, int index) {
  LeafNodeInt *newNode = new LeafNodeInt();
  size_t rightsize = INTARRAYLEAFSIZE - index;
  void *keyptr = &(node->keyArray[index]);
  void *ridptr = &(node->ridArray[index]);
  memcpy(&newNode->keyArray, keyptr, rightsize * sizeof(int));
  memcpy(&newNode->ridArray, ridptr, rightsize * sizeof(RecordId));
  memset(keyptr, 0, rightsize * sizeof(int));
  memset(ridptr, 0, rightsize * sizeof(RecordId));
  return newNode;
}

void insertToNonLeafNode(NonLeafNodeInt *node, int index, int key,
                         PageId pageID) {
  for (int i = INTARRAYNONLEAFSIZE - 1; i > index; i--) {
    node->keyArray[i] = node->keyArray[i - 1];
    node->pageNoArray[i + 1] = node->pageNoArray[i];
  }
  node->keyArray[index] = key;
  node->pageNoArray[index + 1] = pageID;
}

void insertToLeafNode(LeafNodeInt *node, int index, int key, RecordId rid) {
  for (int i = INTARRAYLEAFSIZE - 1; i > index; i--) {
    node->keyArray[i] = node->keyArray[i - 1];
    node->ridArray[i].page_number = node->ridArray[i - 1].page_number;
    node->ridArray[i].slot_number = node->ridArray[i - 1].slot_number;
  }
  node->keyArray[index] = key;
  node->ridArray[index].page_number = rid.page_number;
  node->ridArray[index].slot_number = rid.slot_number;
}

PageId BTreeIndex::createPageForNode(void *node) {
  PageId pid;
  Page *page;
  bufMgr->allocPage(file, pid, page);
  page->set_page_number(pid);
  setNode(page, node);
  bufMgr->unPinPage(file, pid, true);
  return pid;
}

Page *BTreeIndex::getPageByPid(PageId pid) {
  Page *page = nullptr;
  bufMgr->readPage(file, pid, page);
  return (Page *) page;
}

/*******************************  Insert  *******************************/

PageId BTreeIndex::insertToLeafPage(Page *origPage, PageId origPageId,
                                    const int key, const RecordId &rid,
                                    int *midVal) {
  LeafNodeInt *origNode = getLeafNodeFromPage(origPage);
  int index = findInsertionIndexLeaf(origNode, key);

  if (!isLeafNodeFull(origNode)) {
    insertToLeafNode(origNode, index, key, rid);
    setNode(origPage, origNode);
    bufMgr->unPinPage(file, origPageId, true);
    return 0;
  }

  int middleIndex = INTARRAYLEAFSIZE / 2;
  bool insertToLeft = index < middleIndex;
  LeafNodeInt *newNode = splitLeafNode(origNode, middleIndex + insertToLeft);

  if (insertToLeft)
    insertToLeafNode(origNode, index, key, rid);
  else
    insertToLeafNode(newNode, index - middleIndex, key, rid);

  PageId newPageId = createPageForNode(newNode);
  Page *newPage = getPageByPid(newPageId);

  newNode->rightSibPageNo = origNode->rightSibPageNo;
  origNode->rightSibPageNo = newPageId;

  setNode(origPage, origNode);
  setNode(newPage, newNode);
  *midVal = newNode->keyArray[0];

  bufMgr->unPinPage(file, origPageId, true);
  bufMgr->unPinPage(file, newPageId, true);

  return newPageId;
}

PageId BTreeIndex::insertHelper(PageId origPageId, const int key,
                                const RecordId rid, int *midVal) {
  Page *origPage = getPageByPid(origPageId);

  if (isLeaf(origPage))  // base case
    return insertToLeafPage(origPage, origPageId, key, rid, midVal);

  NonLeafNodeInt *origNode = getNonLeafNodeFromPage(origPage);

  PageId origChildPageId =
      origNode->pageNoArray[findIndexNonLeaf(origNode, key)];

  // insert key, rid to child and check whether child is splitted
  int newChildMidVal;
  PageId newChildPageId =
      insertHelper(origChildPageId, key, rid, &newChildMidVal);

  // not split in child
  if (newChildPageId == 0) {
    bufMgr->unPinPage(file, origPageId, false);
    return 0;
  }

  // split in child, need to add splitted child to currNode
  int index = findIndexNonLeaf(origNode, newChildMidVal);
  if (!isNonLeafNodeFull(origNode)) {  // current node is not full
    insertToNonLeafNode(origNode, index, newChildMidVal, newChildPageId);
    setNode(origPage, origNode);
    bufMgr->unPinPage(file, origPageId, true);
    return 0;
  }

  int middleIndex = (INTARRAYNONLEAFSIZE - 1) / 2;

  bool insertToLeft = index < middleIndex;

  // split
  int splitIndex = middleIndex + insertToLeft;
  int insertIndex = insertToLeft ? index : index - middleIndex;

  bool moveKeyUp = !insertToLeft && insertIndex == 0;  // insert to right[0]

  // if we need to move key up, set midVal = key, else key at splited index
  *midVal = moveKeyUp ? newChildMidVal : origNode->keyArray[splitIndex];

  NonLeafNodeInt *newNode = splitNonLeafNode(origNode, splitIndex, moveKeyUp);

  if (!moveKeyUp) {  // need to insert
    NonLeafNodeInt *node = insertToLeft ? origNode : newNode;
    insertToNonLeafNode(node, insertIndex, newChildMidVal, newChildPageId);
  }

  setNode(origPage, origNode);  // save node to orig page
  bufMgr->unPinPage(file, origPageId, true);
  return createPageForNode(newNode);  // return new page
}

/*******************************  Scan  *******************************/

void BTreeIndex::initPageId() {
  currentPageData = getPageByPid(currentPageNum);
  if (isLeaf(currentPageData)) return;

  NonLeafNodeInt *node = getNonLeafNodeFromPage(currentPageData);

  bufMgr->unPinPage(file, currentPageNum, false);
  currentPageNum = node->pageNoArray[findIndexNonLeaf(node, lowValInt)];
  initPageId();
}

void BTreeIndex::moveToNextPage(LeafNodeInt *node) {
  bufMgr->unPinPage(file, currentPageNum, false);
  currentPageNum = node->rightSibPageNo;
  currentPageData = getPageByPid(currentPageNum);
  nextEntry = 0;
}

void BTreeIndex::initEntryIndex() {
  LeafNodeInt *node = getLeafNodeFromPage(currentPageData);
  int entryIndex = findScanIndexLeaf(node, lowValInt, lowOp == GTE);
  if (-1 == entryIndex) {
    moveToNextPage(node);
    return;
  }
  nextEntry = entryIndex;
}

void BTreeIndex::setNextEntry() {
  nextEntry++;
  LeafNodeInt *node = getLeafNodeFromPage(currentPageData);
  if (nextEntry >= INTARRAYLEAFSIZE ||
      node->ridArray[nextEntry].page_number == 0) {
    moveToNextPage(node);
  }
}

/**
 * Constructor
 *
 * The constructor first checks if the specified index ﬁle exists.
 * And index ﬁle name is constructed by concatenating the relational name with
 * the offset of the attribute over which the index is built.
 *
 * If the index ﬁle exists, the ﬁle is opened. Else, a new index ﬁle is created.
 *
 * @param relationName The name of the relation on which to build the index.
 * @param outIndexName The name of the index file.
 * @param bufMgrIn The instance of the global buffer manager.
 * @param attrByteOffset The byte offset of the attribute in the tuple on which
 * to build the index.
 * @param attrType The data type of the attribute we are indexing.
 */
BTreeIndex::BTreeIndex(const string &relationName, string &outIndexName,
                       BufMgr *bufMgrIn, const int attrByteOffset,
                       const Datatype attrType)
    : attrByteOffset(attrByteOffset),
      bufMgr(bufMgrIn),
      attributeType(attrType) {
  outIndexName = getIndexName(relationName, attrByteOffset);

  relationName.copy(indexMetaInfo.relationName, 20, 0);
  indexMetaInfo.attrByteOffset = attrByteOffset;
  indexMetaInfo.attrType = attrType;

  file = new BlobFile(outIndexName, true);

  Page *newPage;
  bufMgr->allocPage(file, indexMetaInfo.rootPageNo, newPage);
  Page *root = (Page *) newPage;

  LeafNodeInt node{};
  setNode(root, &node);
  bufMgr->unPinPage(file, indexMetaInfo.rootPageNo, true);

  FileScan fscan(relationName, bufMgr);
  try {
    RecordId scanRid;
    while (1) {
      fscan.scanNext(scanRid);
      std::string recordStr = fscan.getRecord();
      const char *record = recordStr.c_str();
      int key = *((int *)(record + attrByteOffset));
      insertEntry(&key, scanRid);
    }
  } catch (EndOfFileException e) {
    std::cout << "Read all records" << std::endl;
  }
}

/**
 * Destructor
 *
 * Perform any cleanup that may be necessary, including
 *      clearing up any state variables,
 *      unpinning any B+ Tree pages that are pinned, and
 *      flushing the index file (by calling bufMgr->flushFile()).
 *
 * Note that this method does not delete the index file! But, deletion of the
 * file object is required, which will call the destructor of File class causing
 * the index file to be closed.
 */
BTreeIndex::~BTreeIndex() {
  bufMgr->flushFile(file);
  scanExecuting = false;
  delete file;
}

/**
 * Insert a new entry using the pair <value,rid>.
 * Start from root to recursively find out the leaf to insert the entry in.
 *The insertion may cause splitting of leaf node. This splitting will require
 *addition of new leaf page number entry into the parent non-leaf, which may
 *in-turn get split. This may continue all the way upto the root causing the
 *root to get split. If root gets split, metapage needs to be changed
 *accordingly. Make sure to unpin pages as soon as you can.
 * @param key			Key to insert, pointer to integer/double/char
 *string
 * @param rid			Record ID of a record whose entry is getting
 *inserted into the index.
 **/
const void BTreeIndex::insertEntry(const void *key, const RecordId rid) {
  int splittedPageMidval;
  PageId newPageId = insertHelper(indexMetaInfo.rootPageNo, *(int *)key, rid,
                                  &splittedPageMidval);

  if (newPageId == 0) return;

  PageId pid;
  Page *newPage;
  bufMgr->allocPage(file, pid, newPage);

  NonLeafNodeInt *newRoot = new NonLeafNodeInt();
  newRoot->keyArray[0] = splittedPageMidval;
  newRoot->pageNoArray[0] = indexMetaInfo.rootPageNo;
  newRoot->pageNoArray[1] = newPageId;
  setNode(newPage, newRoot);
  bufMgr->unPinPage(file, pid, true);

  indexMetaInfo.rootPageNo = pid;
}

/**
 *
 * This method is used to begin a filtered scan” of the index.
 *
 * For example, if the method is called using arguments (1,GT,100,LTE), then
 * the scan should seek all entries greater than 1 and less than or equal to
 * 100.
 *
 * @param lowValParm The low value to be tested.
 * @param lowOpParm The operation to be used in testing the low range.
 * @param highValParm The high value to be tested.
 * @param highOpParm The operation to be used in testing the high range.
 */
const void BTreeIndex::startScan(const void *lowValParm,
                                 const Operator lowOpParm,
                                 const void *highValParm,
                                 const Operator highOpParm) {
  if (lowOpParm != GT && lowOpParm != GTE) throw BadOpcodesException();
  if (highOpParm != LT && highOpParm != LTE) throw BadOpcodesException();

  lowValInt = *((int *)lowValParm);
  highValInt = *((int *)highValParm);
  if (lowValInt > highValInt) throw BadScanrangeException();

  lowOp = lowOpParm;
  highOp = highOpParm;

  scanExecuting = true;

  currentPageNum = indexMetaInfo.rootPageNo;

  initPageId();
  initEntryIndex();
}

/**
 * This method fetches the record id of the next tuple that matches the scan
 * criteria. If the scan has reached the end, then it should throw the
 * following exception: IndexScanCompletedException.
 *
 * For instance, if there are two data entries that need to be returned in a
 * scan, then the third call to scanNext must throw
 * IndexScanCompletedException. A leaf page that has been read into the buffer
 * pool for the purpose of scanning, should not be unpinned from buffer pool
 * unless all records from it are read or the scan has reached its end. Use
 * the right sibling page number value from the current leaf to move on to the
 * next leaf which holds successive key values for the scan.
 *
 * @param outRid An output value
 *               This is the record id of the next entry that matches the scan
 * filter set in startScan.
 */
const void BTreeIndex::scanNext(RecordId &outRid) {
  if (!scanExecuting) throw ScanNotInitializedException();
  LeafNodeInt *node = getLeafNodeFromPage(currentPageData);

  outRid.page_number = node->ridArray[nextEntry].page_number;
  outRid.slot_number = node->ridArray[nextEntry].slot_number;

  if (node->ridArray[nextEntry] == RecordId{}) throw IndexScanCompletedException();
  int val = node->keyArray[nextEntry];
  if (val > highValInt) throw IndexScanCompletedException();
  if (val == highValInt && highOp == LT) throw IndexScanCompletedException();
  setNextEntry();
}

/**
 * This method terminates the current scan and unpins all the pages that have
 * been pinned for the purpose of the scan.
 *
 * It throws ScanNotInitializedException when called before a successful
 * startScan call.
 */
const void BTreeIndex::endScan() {
  if (!scanExecuting) throw ScanNotInitializedException();
  scanExecuting = false;
  bufMgr->unPinPage(file, currentPageNum, false);
}
}  // namespace badgerdb
