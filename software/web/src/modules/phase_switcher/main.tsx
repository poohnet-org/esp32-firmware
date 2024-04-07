/* esp32-firmware
 * Copyright (C) 2024 Thomas Hein
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

import * as util from "../../ts/util";
import * as API from "../../ts/api";
import { h, Component } from "preact";
import { Button, ButtonGroup } from "react-bootstrap";
import { __ } from "../../ts/translation";
import { FormRow } from "../../ts/components/form_row";
import { IndicatorGroup  } from "../../ts/components/indicator_group";
import { StatusSection } from "../../ts/components/status_section";
import { CheckCircle, Circle } from "react-feather";

export class PhaseSwitcherStatus extends Component {
    render() {
        if (!util.render_allowed() || !API.hasFeature("phase_switch")) {
            return <StatusSection name="phase_switcher" />;
        }

        let state = API.get('phase_switcher/state');

        return <StatusSection name="phase_switcher">
            <FormRow label={__("phase_switcher.status.cp")}>
                <IndicatorGroup
                    style="width: 100%"
                    class="flex-wrap"
                    value={state.cp_disconnect ? 0 : 1}
                    items={[
                        ["warning", __("phase_switcher.status.disconnected")],
                        ["success", __("phase_switcher.status.connected")]
                    ]}
                />
            </FormRow>

            <FormRow label={__("phase_switcher.status.contactors_requested")} label_muted={__("phase_switcher.status.contactor_names")}>
                <div class="row mx-n1">
                    <IndicatorGroup
                        class="mb-1 col px-1"
                        value={((state.phases_current & 1) == 1) ? 1 : 0}
                        items={[
                            ["secondary", __("phase_switcher.status.off")],
                            ["success", __("phase_switcher.status.on")]
                        ]}
                    />
                    <IndicatorGroup
                        class="mb-1 col px-1"
                        value={((state.phases_current & 2) == 2) ? 1 : 0}
                        items={[
                            ["secondary", __("phase_switcher.status.off")],
                            ["success", __("phase_switcher.status.on")]
                        ]}
                    />
                </div>
            </FormRow>

            <FormRow label={__("phase_switcher.status.contactors_active")} label_muted={__("phase_switcher.status.contactor_names")}>
                <div class="row mx-n1">
                    <IndicatorGroup
                        class="mb-1 col px-1"
                        value={((state.phases_active & 1) == 1) ? 1 : 0}
                        items={[
                            ["secondary", __("phase_switcher.status.off")],
                            ["success", __("phase_switcher.status.on")]
                        ]}
                    />
                    <IndicatorGroup
                        class="mb-1 col px-1"
                        value={((state.phases_active & 2) == 2) ? 1 : 0}
                        items={[
                            ["secondary", __("phase_switcher.status.off")],
                            ["success", __("phase_switcher.status.on")]
                        ]}
                    />
                </div>
            </FormRow>
        </StatusSection>;
    }
}

export function pre_init() {
}

export function init() {
}
