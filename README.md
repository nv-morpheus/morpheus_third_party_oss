<!--
SPDX-FileCopyrightText: Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Source Code for OSS Packages in the NVIDIA Morpheus Docker container

This repository contains the source code for OSS packages which are included in the NVIDIA Morpheus Docker image. This repository does not include packages which were already present in the base [`nvidia/cuda`](https://hub.docker.com/r/nvidia/cuda) image.

Branches in this repository correspond to the versions of the OSS packages used by NVIDIA Morpheus, ex: `branch-24.10` corresponds to version `24.10` of Morpheus and `branch-25.02` will correspond with version `25.02` of Morpheus.

The source code is organized in the following directory structure:
```
third_party/
├── package_name
│   ├── package_version
│   │   ├── package_source_code
```

Source code provided for each package is provided without any modifications.