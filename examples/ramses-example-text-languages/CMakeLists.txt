#  -------------------------------------------------------------------------
#  Copyright (C) 2018 BMW Car IT GmbH
#  -------------------------------------------------------------------------
#  This Source Code Form is subject to the terms of the Mozilla Public
#  License, v. 2.0. If a copy of the MPL was not distributed with this
#  file, You can obtain one at https://mozilla.org/MPL/2.0/.
#  -------------------------------------------------------------------------

MODULE_WITH_SHARED_LIBRARY(

    #==========================================================================
    # general module information
    #==========================================================================
    NAME                    ramses-example-text-languages
    TYPE                    BINARY
    ENABLE_INSTALL          OFF    # disabled installation to save space in release tarball

    #==========================================================================
    # files of this module
    #==========================================================================
    FILES_SOURCE            src/main.cpp
    RESOURCE_FOLDER         res
)
