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

This repository contains the source code for OSS packages which are included in the NVIDIA Morpheus Docker images. 

The source code archives are organized in the following directory structure:
```
third_party/
├── release_version
│   ├── image_name-sha256-<SHA HASH>.tar
```

Where the <SHA HASH> is the output of the `sha256sum` command on the image archive.

Source code provided for each package is provided without any modifications.