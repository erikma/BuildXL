// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

import * as BuildXLSdk from "Sdk.BuildXL";
import * as Deployment from "Sdk.Deployment";
import * as Managed from "Sdk.Managed";
import { NetFx } from "Sdk.BuildXL";
import {Transformer} from "Sdk.Transformers";

namespace Rush {
    @@public
    export const dll = BuildXLSdk.library({
        assemblyName: "BuildXL.FrontEnd.Rush",
        generateLogs: true,
        sources: globR(d`.`, "*.cs"),
        references: [
            ...BuildXLSdk.tplPackages,
            importFrom("BuildXL.Engine").Cache.dll,
            importFrom("BuildXL.Engine").Processes.dll,
            importFrom("BuildXL.Pips").dll,
            importFrom("BuildXL.Utilities").dll,
            importFrom("BuildXL.Utilities").Collections.dll,
            importFrom("BuildXL.Utilities").Configuration.dll,
            importFrom("BuildXL.Utilities").Script.Constants.dll,
            importFrom("BuildXL.Utilities").Storage.dll,
            importFrom("BuildXL.Utilities").Script.Constants.dll,
            importFrom("BuildXL.Utilities").Native.dll,
            importFrom("Newtonsoft.Json").pkg,
            Utilities.dll,
            SdkProjectGraph.dll,
            TypeScript.Net.dll,
            Script.dll,
            Core.dll,
            Sdk.dll,
        ],
        runtimeContent:[
            importFrom("BuildXL.Tools").RushGraphBuilder.deployment
        ],
        internalsVisibleTo: [
            "Test.BuildXL.FrontEnd.Rush",
        ],
    });
}
