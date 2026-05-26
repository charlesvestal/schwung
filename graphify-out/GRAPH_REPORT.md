# Graph Report - schwung  (2026-05-29)

## Corpus Check
- 337 files · ~984,528 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 5543 nodes · 17836 edges · 40 communities detected
- Extraction: 78% EXTRACTED · 22% INFERRED · 0% AMBIGUOUS · INFERRED: 3900 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Community 0|Community 0]]
- [[_COMMUNITY_Community 1|Community 1]]
- [[_COMMUNITY_Community 2|Community 2]]
- [[_COMMUNITY_Community 3|Community 3]]
- [[_COMMUNITY_Community 4|Community 4]]
- [[_COMMUNITY_Community 5|Community 5]]
- [[_COMMUNITY_Community 6|Community 6]]
- [[_COMMUNITY_Community 7|Community 7]]
- [[_COMMUNITY_Community 8|Community 8]]
- [[_COMMUNITY_Community 9|Community 9]]
- [[_COMMUNITY_Community 10|Community 10]]
- [[_COMMUNITY_Community 11|Community 11]]
- [[_COMMUNITY_Community 12|Community 12]]
- [[_COMMUNITY_Community 13|Community 13]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 15|Community 15]]
- [[_COMMUNITY_Community 16|Community 16]]
- [[_COMMUNITY_Community 17|Community 17]]
- [[_COMMUNITY_Community 18|Community 18]]
- [[_COMMUNITY_Community 19|Community 19]]
- [[_COMMUNITY_Community 20|Community 20]]
- [[_COMMUNITY_Community 21|Community 21]]
- [[_COMMUNITY_Community 22|Community 22]]
- [[_COMMUNITY_Community 23|Community 23]]
- [[_COMMUNITY_Community 24|Community 24]]
- [[_COMMUNITY_Community 25|Community 25]]
- [[_COMMUNITY_Community 26|Community 26]]
- [[_COMMUNITY_Community 27|Community 27]]
- [[_COMMUNITY_Community 28|Community 28]]
- [[_COMMUNITY_Community 29|Community 29]]
- [[_COMMUNITY_Community 30|Community 30]]
- [[_COMMUNITY_Community 31|Community 31]]
- [[_COMMUNITY_Community 32|Community 32]]
- [[_COMMUNITY_Community 34|Community 34]]
- [[_COMMUNITY_Community 35|Community 35]]
- [[_COMMUNITY_Community 37|Community 37]]
- [[_COMMUNITY_Community 38|Community 38]]
- [[_COMMUNITY_Community 39|Community 39]]
- [[_COMMUNITY_Community 41|Community 41]]
- [[_COMMUNITY_Community 44|Community 44]]

## God Nodes (most connected - your core abstractions)
1. `JS_IsException()` - 338 edges
2. `JS_NewInt32()` - 150 edges
3. `JS_ThrowTypeError()` - 141 edges
4. `JS_IsUndefined()` - 127 edges
5. `assert()` - 105 edges
6. `handleSelect()` - 103 edges
7. `JS_FreeCString()` - 98 edges
8. `print()` - 96 edges
9. `JS_ToInt32()` - 93 edges
10. `JS_ToCString()` - 87 edges

## Surprising Connections (you probably didn't know these)
- `JS_ToBool()` --calls--> `js_host_ext_midi_remap_enable()`  [INFERRED]
  libs/quickjs/quickjs-2025-04-26/quickjs.c → src/shadow/shadow_ui.c
- `JS_ToFloat64()` --calls--> `js_tts_set_speed()`  [INFERRED]
  libs/quickjs/quickjs-2025-04-26/quickjs.c → src/shadow/shadow_ui.c
- `JS_ToFloat64()` --calls--> `js_tts_set_pitch()`  [INFERRED]
  libs/quickjs/quickjs-2025-04-26/quickjs.c → src/shadow/shadow_ui.c
- `JS_NewInt32()` --calls--> `js_host_get_refresh_rate()`  [INFERRED]
  libs/quickjs/quickjs-2025-04-26/quickjs.h → src/schwung_host.c
- `JS_NewInt32()` --calls--> `js_shadow_get_ui_flags()`  [INFERRED]
  libs/quickjs/quickjs-2025-04-26/quickjs.h → src/shadow/shadow_ui.c

## Communities

### Community 0 - "Community 0"
Cohesion: 0.01
Nodes (627): drawComponentPicker(), drawComponentSelector(), drawConfirmDelete(), drawEditorOverview(), drawKnobEditor(), drawParamEditor(), drawSlotMenu(), drawUI() (+619 more)

### Community 1 - "Community 1"
Cohesion: 0.01
Nodes (649): js_point_finalizer(), js_point_get_xy(), js_point_norm(), js_point_set_xy(), get_hi_surrogate(), get_lo_surrogate(), max_uint32(), min_uint32() (+641 more)

### Community 2 - "Community 2"
Cohesion: 0.01
Nodes (658): js_point_ctor(), max_int(), max_int64(), min_int64(), list_add_tail(), add_fast_array_element(), add_gc_object(), add_property() (+650 more)

### Community 3 - "Community 3"
Cohesion: 0.01
Nodes (355): lfo_advance_phase(), lfo_compute_shape(), lfo_migrate_division_index(), lfo_process_midi(), lfo_rand_bipolar(), lfo_sync_rate_hz(), capture_apply_group(), capture_clear() (+347 more)

### Community 4 - "Community 4"
Cohesion: 0.01
Nodes (344): analytics_diff_modules(), analytics_enabled(), analytics_init(), analytics_set_enabled(), analytics_track(), find_in_snapshot(), generate_uuid_v4(), load_or_create_id() (+336 more)

### Community 5 - "Community 5"
Cohesion: 0.02
Nodes (232): assert(), ColorPoint, main(), bswap16(), bswap32(), bswap64(), dbuf_error(), dbuf_free() (+224 more)

### Community 6 - "Community 6"
Cohesion: 0.02
Nodes (130): CSRFProtection(), CSRFProtectionWithExemptions(), generateCSRFToken(), PathTraversalProtection(), SecurityHeaders(), ValidatePath(), App, AssetFile (+122 more)

### Community 7 - "Community 7"
Cohesion: 0.02
Nodes (214): fib(), js_fib(), js_fib_init(), JS_INIT_MODULE(), js_init_module(), js_point_init(), js_display_register_bindings(), has_suffix() (+206 more)

### Community 8 - "Community 8"
Cohesion: 0.03
Nodes (193): load_jpeg_image(), stbi__addints_valid(), stbi__addsizes_valid(), stbi__at_eof(), stbi__bit_reverse(), stbi__bitcount(), stbi__bitreverse16(), stbi__blinn_8x8() (+185 more)

### Community 9 - "Community 9"
Cohesion: 0.02
Nodes (117): JackRequest(), JackResult(), Read(), Size(), Write(), clear_display(), draw_char(), draw_string() (+109 more)

### Community 10 - "Community 10"
Cohesion: 0.04
Nodes (136): js_display_load_ttf_font(), equal(), main(), my_stbtt_initfont(), my_stbtt_print(), stbrp_init_target(), stbrp_pack_rects(), stbtt__add_point() (+128 more)

### Community 11 - "Community 11"
Cohesion: 0.04
Nodes (134): bounded_strstr(), calc_knob_accel(), chain_get_clock_status(), chain_log(), chain_mod_alloc_target_entry(), chain_mod_apply_effective_value(), chain_mod_clampf(), chain_mod_clear_source() (+126 more)

### Community 12 - "Community 12"
Cohesion: 0.03
Nodes (91): Jack(), clearLEDs(), fillPads(), getOctaveDisplay(), getPadColor(), setupLedBatch(), updateStatusLine(), adjustParam() (+83 more)

### Community 13 - "Community 13"
Cohesion: 0.11
Nodes (103): _(), A(), ae(), ar(), at(), B(), be(), br() (+95 more)

### Community 14 - "Community 14"
Cohesion: 0.06
Nodes (75): clz32(), clz64(), ctz32(), float64_as_uint64(), min_int(), uint64_as_float64(), build_mul_log2_radix_table(), dtoa_free() (+67 more)

### Community 15 - "Community 15"
Cohesion: 0.03
Nodes (19): bench(), bigint256_arith(), bigint32_arith(), bigint64_arith(), bigint_arith(), func_call(), func_closure_call(), g() (+11 more)

### Community 16 - "Community 16"
Cohesion: 0.07
Nodes (70): fmt(), performCoreUpdate_disabled(), applyDisplayFormat(), clampValue(), connect(), createKnobSVG(), degToRad(), describeArc() (+62 more)

### Community 17 - "Community 17"
Cohesion: 0.06
Nodes (70): link_audio_drain_avail_stats(), chown_to_ableton(), load_engine_choice(), save_engine_choice(), tts_cleanup(), tts_get_audio(), tts_get_enabled(), tts_get_pitch() (+62 more)

### Community 18 - "Community 18"
Cohesion: 0.06
Nodes (62): accept_line(), alert(), backward_char(), backward_delete_char(), backward_kill_line(), backward_kill_word(), backward_word(), cmd_readline_start() (+54 more)

### Community 19 - "Community 19"
Cohesion: 0.06
Nodes (61): buildChainJson(), createEditorState(), createEmptyKnobs(), deleteChain(), disableComponentHostRouting(), disableSourceHostRouting(), drawKnobParamPicker(), enableComponentHostRouting() (+53 more)

### Community 20 - "Community 20"
Cohesion: 0.09
Nodes (40): barLengthModeLabel(), countNonEmpty(), describeEntry(), drainInjectQueue(), drawListView(), drawStepParamsView(), ensureEntryExists(), ensureTrailingEmpty() (+32 more)

### Community 21 - "Community 21"
Cohesion: 0.13
Nodes (28): assert(), assert_json_error(), assert_throws(), check_error_pos(), eval_error(), get_string_pos(), rope_concat(), test() (+20 more)

### Community 22 - "Community 22"
Cohesion: 0.12
Nodes (2): ShmConfig, boolU8()

### Community 23 - "Community 23"
Cohesion: 0.13
Nodes (19): audio_metrics(), decode_stereo_f32(), ffprobe_stream(), main(), parse_args(), print_summary(), resolve_inputs(), compareVersions() (+11 more)

### Community 24 - "Community 24"
Cohesion: 0.17
Nodes (21): biquad_process(), biquad_reset(), biquad_set_high_shelf(), biquad_set_highpass(), biquad_set_lowpass(), biquad_set_notch(), biquad_set_passthrough(), biquad_set_riaa_stage1() (+13 more)

### Community 25 - "Community 25"
Cohesion: 0.18
Nodes (19): assert(), test_do_while(), test_for(), test_for_break(), test_for_in(), test_for_in2(), test_for_in_proxy(), test_switch1() (+11 more)

### Community 26 - "Community 26"
Cohesion: 0.18
Nodes (14): arp_add_note(), arp_calc_clocks_per_step(), arp_calc_samples_per_step(), arp_get_next_note(), arp_get_param(), arp_get_sync_warning(), arp_process_midi(), arp_query_clock_status() (+6 more)

### Community 27 - "Community 27"
Cohesion: 0.26
Nodes (12): buildHierarchy(), fetchAndParseManual(), getFetchedTime(), isStale(), parseHtml(), processDownloadedHtml(), readManualJson(), refreshManualBackground() (+4 more)

### Community 28 - "Community 28"
Cohesion: 0.2
Nodes (8): jack_slist_alloc(), jack_slist_append(), jack_slist_copy(), jack_slist_free(), jack_slist_last(), jack_slist_remove(), jack_slist_sort(), jack_slist_sort_merge()

### Community 29 - "Community 29"
Cohesion: 0.24
Nodes (8): destroy_instance(), emit_note_off(), emit_step(), inject(), recompute_step_samples(), refresh_bpm(), render_block(), set_param()

### Community 30 - "Community 30"
Cohesion: 0.25
Nodes (12): allpass_init(), allpass_process(), comb_init(), comb_process(), json_get_float(), move_audio_fx_init_v2(), v2_create_instance(), v2_destroy_instance() (+4 more)

### Community 31 - "Community 31"
Cohesion: 0.18
Nodes (5): json_get_int(), move_midi_fx_init(), velocity_scale_set_param(), fail(), main()

### Community 32 - "Community 32"
Cohesion: 0.24
Nodes (5): chord_process_midi(), chord_set_param(), json_get_int(), json_get_string(), queue_note()

### Community 34 - "Community 34"
Cohesion: 0.36
Nodes (4): getParamTarget(), groupParamsByTarget(), parseParamKey(), stripParamPrefix()

### Community 35 - "Community 35"
Cohesion: 0.54
Nodes (7): applyDisplayFormat(), fmtHz(), fmtPercent(), fmtSemitones(), formatParamForSet(), formatParamValue(), precisionForStep()

### Community 37 - "Community 37"
Cohesion: 0.6
Nodes (4): GetShmAddress(), Init(), Jack(), SetShmIndex()

### Community 38 - "Community 38"
Cohesion: 0.5
Nodes (3): DEC_ATOMIC(), INC_ATOMIC(), CAS()

### Community 39 - "Community 39"
Cohesion: 0.83
Nodes (3): main(), read_cache(), write_cache()

### Community 41 - "Community 41"
Cohesion: 0.83
Nodes (3): jack_default_server_name(), jack_varargs_init(), jack_varargs_parse()

### Community 44 - "Community 44"
Cohesion: 1.0
Nodes (2): makeId(), requestFromParent()

## Knowledge Gaps
- **34 isolated node(s):** `Convert a chain_params entry to a ui_hierarchy param object.`, `Parse a BDF font file and extract glyph bitmaps for ASCII 32-126.      Returns (`, `Generate a deployment PNG + .dat from a BDF font file.      Output format is ide`, `Generate a font PNG for deployment with the host's bitmap font loader.      Form`, `ParamChange` (+29 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **Thin community `Community 22`** (32 nodes): `ShmConfig`, `boolU8()`, `.DisplayMirror()`, `.getF32()`, `.getU16()`, `.getU8()`, `shmconfig.go`, `.OverlayKnobsMode()`, `.SetDisplayMirror()`, `.setF32()`, `.SetOpenToolCmd()`, `.SetOverlayKnobsMode()`, `.SetPagesEnabled()`, `.SetSetPagesEnabled()`, `.SetSkipbackRequireVolume()`, `.SetSkipbackSeconds()`, `.SetTTSDebounce()`, `.SetTTSEnabled()`, `.SetTTSEngine()`, `.SetTTSPitch()`, `.SetTTSSpeed()`, `.SetTTSVolume()`, `.setU16()`, `.setU8()`, `.SkipbackRequireVolume()`, `.SkipbackSeconds()`, `.TTSDebounce()`, `.TTSEnabled()`, `.TTSEngine()`, `.TTSPitch()`, `.TTSSpeed()`, `.TTSVolume()`
  Too small to be a meaningful cluster - may be noise or needs more connections extracted.
- **Thin community `Community 44`** (3 nodes): `schwung-remote-api.js`, `makeId()`, `requestFromParent()`
  Too small to be a meaningful cluster - may be noise or needs more connections extracted.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `error()` connect `Community 6` to `Community 5`, `Community 7`, `Community 15`, `Community 17`, `Community 21`, `Community 25`?**
  _High betweenness centrality (0.138) - this node is a cross-community bridge._
- **Why does `assert()` connect `Community 5` to `Community 1`, `Community 2`, `Community 4`, `Community 6`, `Community 7`, `Community 14`?**
  _High betweenness centrality (0.132) - this node is a cross-community bridge._
- **Why does `print()` connect `Community 0` to `Community 4`, `Community 6`, `Community 9`, `Community 10`, `Community 12`, `Community 20`, `Community 23`?**
  _High betweenness centrality (0.129) - this node is a cross-community bridge._
- **Are the 337 inferred relationships involving `JS_IsException()` (e.g. with `JS_ExecutePendingJob()` and `JS_NewAtomLen()`) actually correct?**
  _`JS_IsException()` has 337 INFERRED edges - model-reasoned connections that need verification._
- **Are the 147 inferred relationships involving `JS_NewInt32()` (e.g. with `JS_AtomIsNumericIndex1()` and `JS_NewObjectFromShape()`) actually correct?**
  _`JS_NewInt32()` has 147 INFERRED edges - model-reasoned connections that need verification._
- **Are the 30 inferred relationships involving `JS_ThrowTypeError()` (e.g. with `js_agent_start()` and `js_agent_leaving()`) actually correct?**
  _`JS_ThrowTypeError()` has 30 INFERRED edges - model-reasoned connections that need verification._
- **Are the 126 inferred relationships involving `JS_IsUndefined()` (e.g. with `JS_AtomIsNumericIndex()` and `JS_IsInstanceOf()`) actually correct?**
  _`JS_IsUndefined()` has 126 INFERRED edges - model-reasoned connections that need verification._