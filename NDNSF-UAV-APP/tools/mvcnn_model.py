#!/usr/bin/env python3
"""Small, self-contained MVCNN-family model used by the UAV CPU subject.

The model deliberately keeps the view dimension explicit: one shared 2-D
backbone extracts a feature for every view, then symmetric masked max pooling
produces one joint representation and one fused classifier decision.  It is
not a per-view detector or a majority-vote adapter.
"""

from __future__ import annotations

import torch
from torch import Tensor, nn


class TinyMVCNN(nn.Module):
    """A compact MVCNN suitable for deterministic CPU qualification."""

    def __init__(self, class_count: int = 3, feature_dim: int = 32) -> None:
        super().__init__()
        self.backbone = nn.Sequential(
            nn.Conv2d(3, 8, kernel_size=5, stride=2, padding=2),
            nn.ReLU(inplace=False),
            nn.Conv2d(8, 16, kernel_size=3, stride=2, padding=1),
            nn.ReLU(inplace=False),
            nn.Conv2d(16, feature_dim, kernel_size=3, stride=2, padding=1),
            nn.ReLU(inplace=False),
            nn.AdaptiveAvgPool2d((1, 1)),
        )
        self.classifier = nn.Linear(feature_dim, class_count)

    def forward(self, images: Tensor, view_mask: Tensor) -> tuple[Tensor, Tensor]:
        if images.ndim != 5:
            raise ValueError("images must have shape [batch, views, channels, height, width]")
        if view_mask.ndim != 2:
            raise ValueError("view_mask must have shape [batch, views]")
        batch, views = images.shape[:2]
        if view_mask.shape != (batch, views):
            raise ValueError("view_mask shape does not match images")
        flattened = images.reshape(batch * views, *images.shape[2:])
        features = self.backbone(flattened).reshape(batch, views, -1)
        mask = view_mask.to(dtype=features.dtype).unsqueeze(-1)
        negative_inf = torch.full_like(features, -1.0e9)
        masked_features = torch.where(mask > 0.5, features, negative_inf)
        pooled = torch.amax(masked_features, dim=1)
        logits = self.classifier(pooled)
        return logits, pooled


def make_model(class_count: int = 3, feature_dim: int = 32) -> TinyMVCNN:
    return TinyMVCNN(class_count=class_count, feature_dim=feature_dim)

