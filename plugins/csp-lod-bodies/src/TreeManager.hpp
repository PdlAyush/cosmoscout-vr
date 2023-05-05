////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#ifndef CSP_LOD_BODIES_TREEMANAGEr_HPP
#define CSP_LOD_BODIES_TREEMANAGEr_HPP

#include "TileId.hpp"
#include "TileQuadTree.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace csp::lodbodies {

struct PlanetParameters;
class TileNode;
class TileSource;
class TileDataBase;
class GLResources;
class TileTextureArray;

/// Manages a TileQuadTree and TileNode requested from a TileSource as well as data (TileDataBase)
/// associated with each TileNode.
///
/// Tiles to load from the configured TileSource are passed in with a call to request and previously
/// (asynchronously) loaded tiles are merged into the TileQuadTree with a call to update.
///
/// In addition to managing the loading of tiles and inserting them into the managed TileQuadTree
/// this also keeps track of the "age" of nodes. A nodes age is measured in frames since the last
/// time it was used - other classes mark nodes as used (e.g. LODVisitor when testing visibility of
/// a node).
///
/// In order to quickly find "old" nodes a vector of pointers (AgeStore) to the entries of the
/// RenderDataMap is used (this is possible because unordered_map guarantees that pointers to values
/// do not change, even when rehashing occurs). The AgeStore is sorted so that the oldest nodes are
/// at the back and those are removed if their age exceeds a certain threshold (see
/// TreeManager::prune).
class TreeManager {
 public:
  explicit TreeManager(PlanetParameters const& params, std::shared_ptr<GLResources> glResources);

  TreeManager(TreeManager const& other) = delete;
  TreeManager(TreeManager&& other)      = delete;

  TreeManager& operator=(TreeManager const& other) = delete;
  TreeManager& operator=(TreeManager&& other) = delete;

  virtual ~TreeManager() = default;

  /// Set tile source src to use.
  void setSource(TileSource* src);

  /// Returns currently used tile source.
  TileSource* getSource() const;

  /// Returns pointer to the TileQuadTree managed by this.
  TileQuadTree* getTree();

  /// Sets a name for this in order to distinguish it from other instances The name is used in
  /// debug/information messages for example.
  void setName(std::string const& name);

  /// Returns the name for this instance.
  std::string const& getName() const;

  /// Request data tiles with indices tileIds to be loaded and queued to be merged into the quad
  /// tree (with a subsequent call to update).
  void request(std::vector<TileId> const& tileIds);

  /// Update the TileQuadTree managed by this with the tiles that have been loaded from the
  /// TileSource since the last call to update.
  void update();

  /// Removes all nodes from the tree and frees data associated with them.
  void clear();

  int  getFrameCount() const;
  void setFrameCount(int frameCount);

  /// Returns a pointer to the TileTextureArray used by this to manage texture data. This is an
  /// internal interface for use by TileRenderer.
  TileTextureArray& getTileTextureArray() const;

  /// Returns the number of nodes in the tree managed by this.
  std::size_t getNodeCount() const;

  /// Returns the number of nodes uploaded to the GPU.
  std::size_t getNodeCountGPU() const;

 private:
  using RDMapValue = std::unordered_map<TileId, TileNode*>::value_type;
  using AgeStore   = std::vector<RDMapValue*>;

  struct AgeLess;

  /// Tracks a node and the frame it was loaded in - for nodes that can not immediately be merged.
  struct NodeAge {
    explicit NodeAge(TileNode* node, int frame);

    TileNode* mNode;
    int       mFrame;
  };

  /// Used as a callback for the TileSource to call when a node is loaded.
  void onNodeLoaded(TileSource* source, int level, glm::int64 patchIdx, TileNode* node);

  /// Helper function to handle processing after node is successfully inserted into the managed
  /// TileQuadTree.
  void onNodeInserted(TileNode* node);

  /// Helper function to free resources associated with rdata.
  void releaseResources(TileDataBase* rdata);

  /// Remove nodes from the managed TileQuadTree that have not been used for a number of frames.
  /// Sort tiles by age (frames since last use, see TreeManager::AgeLess for details) and
  /// removes those considered too "old".
  void prune();

  /// Merge nodes loaded since the last merge into the managed TileQuadTree. It is possible that a
  /// loaded node can not be inserted into the tree, for example because its parent has been removed
  /// in the meantime. These "unmerged" nodes are kept around in unmergedNodes_ (see
  /// TreeManager::storeUnmerged) for a few frames, in case the parent node is loaded in the
  /// meantime. If this "grace period" has expired and the node still cannot be inserted into the
  /// tree it is deleted (see TreeManager::mergeUnmerged).
  void merge();

  PlanetParameters const*               mParams;
  std::shared_ptr<GLResources>          mGlMgr;
  std::unordered_map<TileId, TileNode*> mRdMap;
  AgeStore                              mAgeStore;

  TileQuadTree mTree;
  TileSource*  mSrc;

  std::unordered_set<TileId> mPendingTiles;
  std::vector<NodeAge>       mUnmergedNodes;

  std::mutex             mLoadedMtx;
  std::vector<TileNode*> mLoadedNodes;

  std::string mName;
  int         mFrameCount;
  bool        mAsyncLoading;
};

} // namespace csp::lodbodies

#endif // CSP_LOD_BODIES_TREEMANAGEr_HPP
