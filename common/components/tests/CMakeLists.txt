# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

eclipsa_add_test(test_export_error_banner ExportErrorBanner_test.cpp "components")

# B5 / V2-01-UI — the midnight-scope panner pad. Also renders the parity
# screenshots when FRIDAY_B5_SHOTS is set.
eclipsa_add_test(test_friday_panner_scope FridayPannerScope_test.cpp "components;processors")

# §B6-3 — room-view speaker geometry, held to the layout KALA renders.
eclipsa_add_test(test_room_view_speakers RoomViewSpeakers_test.cpp "components;processors")
