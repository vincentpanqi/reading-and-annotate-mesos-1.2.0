// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __TEST_DOCKER_ARCHIVE_HPP__
#define __TEST_DOCKER_ARCHIVE_HPP__

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/command_utils.hpp"

#include "tests/containerizer/rootfs.hpp"

using process::Failure;
using process::Future;

namespace mesos {
namespace internal {
namespace tests {

// This represents a docker archive. It has the same format
// as that tarball generated by doing a 'docker save'.
class DockerArchive
{
public:
  // Create a docker test image tarball in docker registry directory.
  // Users can define own entrypoint/cmd as JSON array of JSON string
  // (e.g., `[\"sh\", \"-c\"]`).
  //
  // NOTE: The default value for `environment` includes some environment
  // variables which will cause problems if they are fed into one of Mesos'
  // built-in executors. This is on purpose, as the environment variables
  // of the image should not be passed into built-in executors. Tests that
  // use a custom executor should consider overriding this default.
  static Future<Nothing> create(
      const std::string& directory,
      const std::string& name,
      const std::string& entrypoint = "null",
      const std::string& cmd = "null",
      const std::vector<std::string>& environment = {
        {"LD_LIBRARY_PATH=invalid"},
        {"LIBPROCESS_IP=invalid"},
        {"LIBPROCESS_PORT=invalid"}})
  {
    Try<Nothing> mkdir = os::mkdir(directory, true);
    if (mkdir.isError()) {
      return Failure("Failed to create '" + directory + "': " + mkdir.error());
    }

    const std::string imagePath = path::join(directory, name);

    mkdir = os::mkdir(imagePath);
    if (mkdir.isError()) {
      return Failure("Failed to create docker test image directory '" +
                     imagePath + "': " + mkdir.error());
    }

    const std::string layerId =
      "815b809d588c80fd6ddf4d6ac244ad1c01ae4cbe0f91cc7480e306671ee9c346";

    const std::string layerPath = path::join(imagePath, layerId);

    // Create docker test image `repositories`.
    JSON::Value repositories = JSON::parse(strings::format(
        R"~(
        {
            "%s": {
                "latest": "%s"
            }
        })~",
        name,
        layerId).get()).get();

    Try<Nothing> write = os::write(
        path::join(imagePath, "repositories"),
        stringify(repositories));

    if (write.isError()) {
      return Failure("Failed to save docker test image 'repositories': " +
                     write.error());
    }

    mkdir = os::mkdir(layerPath);
    if (mkdir.isError()) {
      return Failure("Failed to create docker test image layer '" +
                     layerId + "': " + mkdir.error());
    }

    JSON::Value manifest = JSON::parse(strings::format(
        R"~(
        {
            "id": "815b809d588c80fd6ddf4d6ac244ad1c01ae4cbe0f91cc7480e306671ee9c346",
            "created": "2016-03-02T17:16:00.167415955Z",
            "container": "eb53609036555d26c39bdccfa9850426934bdfde96111d099041689b2251a377",
            "container_config": {
                "Hostname": "eb5360903655",
                "Domainname": "",
                "User": "",
                "AttachStdin": false,
                "AttachStdout": false,
                "AttachStderr": false,
                "Tty": false,
                "OpenStdin": false,
                "StdinOnce": false,
                "Env": null,
                "Cmd": [
                    "/bin/sh",
                    "-c",
                    "#(nop) ADD file:81ba6f20bdb99e6c13c434a577069860b6656908031162083b1ac9c02c71dd9f in /"
                ],
                "Image": "",
                "Volumes": null,
                "WorkingDir": "",
                "Entrypoint": null,
                "OnBuild": null,
                "Labels": null
            },
            "docker_version": "1.9.1",
            "config": {
                "Hostname": "eb5360903655",
                "Domainname": "",
                "User": "",
                "AttachStdin": false,
                "AttachStdout": false,
                "AttachStderr": false,
                "Tty": false,
                "OpenStdin": false,
                "StdinOnce": false,
                "Env": %s,
                "Cmd": %s,
                "Image": "",
                "Volumes": null,
                "WorkingDir": "",
                "Entrypoint": %s,
                "OnBuild": null,
                "Labels": null
            },
            "architecture": "amd64",
            "os": "linux"
        })~",
        std::string(jsonify(environment)),
        cmd,
        entrypoint).get()).get();

    write = os::write(
        path::join(layerPath, "json"),
        stringify(manifest));

    if (write.isError()) {
      return Failure("Failed to save docker test image layer '" + layerId +
                     "': " + write.error());
    }

    const std::string rootfsDir = path::join(layerPath, "layer");

    mkdir = os::mkdir(rootfsDir);
    if (mkdir.isError()) {
      return Failure("Failed to create layer rootfs directory '" +
                     rootfsDir + "': " + mkdir.error());
    }

    // Create one linux rootfs for the layer.
    Try<process::Owned<Rootfs>> rootfs = LinuxRootfs::create(rootfsDir);
    if (rootfs.isError()) {
      return Failure("Failed to create docker test image rootfs: " +
                     rootfs.error());
    }

    Future<Nothing> tarRootfs = command::tar(
        Path("."),
        Path(path::join(layerPath, "layer.tar")),
        rootfsDir);

    tarRootfs.await();

    if (!tarRootfs.isReady()) {
      return Failure(
          "Failed to tar root filesystem: " +
          (tarRootfs.isFailed() ? tarRootfs.failure() : "discarded"));
    }

    Try<Nothing> rmdir = os::rmdir(rootfsDir);
    if (rmdir.isError()) {
        return Failure("Failed to remove layer rootfs directory: " +
                       rmdir.error());
    }

    write = os::write(
        path::join(layerPath, "VERSION"),
        "1.0");

    if (write.isError()) {
      return Failure("Failed to save layer version: " + write.error());
    }

    Future<Nothing> tarImage = command::tar(
        Path("."),
        Path(path::join(directory, name + ".tar")),
        imagePath);

    tarImage.await();

    if (!tarImage.isReady()) {
      return Failure(
          "Failed to tar docker test image: " +
          (tarImage.isFailed() ? tarImage.failure() : "discarded"));
    }

    rmdir = os::rmdir(imagePath);
    if (rmdir.isError()) {
      return Failure("Failed to remove image directory: " +
                     rmdir.error());
    }

    return Nothing();
  }
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_DOCKER_ARCHIVE_HPP__