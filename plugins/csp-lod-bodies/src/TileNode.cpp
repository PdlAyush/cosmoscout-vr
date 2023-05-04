////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#include "TileNode.hpp"

namespace csp::lodbodies {

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode::TileNode()
    : mTile() {
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode::TileNode(TileBase* tile)
    : mTile(tile)
    , mParent(nullptr) {
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode::TileNode(std::unique_ptr<TileBase>&& tile)
    : mTile(std::move(tile))
    , mParent(nullptr) {
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int TileNode::getLevel() const {
  return mTile->getLevel();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

glm::int64 TileNode::getPatchIdx() const {
  return mTile->getPatchIdx();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileId const& TileNode::getTileId() const {
  return mTile->getTileId();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::type_info const& TileNode::getTileTypeId() const {
  return mTile->getTypeId();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileBase* TileNode::getTile() const {
  return mTile.get();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileBase* TileNode::releaseTile() {
  return mTile.release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void TileNode::setTile(std::unique_ptr<TileBase> tile) {
  mTile = std::move(tile);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode* TileNode::getChild(int childIdx) const {
  return mChildren.at(childIdx).get();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode* TileNode::releaseChild(int childIdx) {
  if (mChildren.at(childIdx)) {
    mChildren.at(childIdx)->setParent(nullptr);
  }

  return mChildren.at(childIdx).release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void TileNode::setChild(int childIdx, TileNode* child) {
  // unset OLD parent
  if (mChildren.at(childIdx)) {
    mChildren.at(childIdx)->setParent(nullptr);
  }

  mChildren.at(childIdx).reset(child);

  // set NEW parent
  if (mChildren.at(childIdx)) {
    mChildren.at(childIdx)->setParent(this);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TileNode* TileNode::getParent() const {
  return mParent;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void TileNode::setParent(TileNode* parent) {
  mParent = parent;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool isRefined(TileNode const& node) {
  return node.getChild(0) && node.getChild(1) && node.getChild(2) && node.getChild(3);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csp::lodbodies
