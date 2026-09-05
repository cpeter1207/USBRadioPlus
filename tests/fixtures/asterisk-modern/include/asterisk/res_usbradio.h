/* Build-system fixture: the shared-device API identifies modern ASL3. */
enum ast_radio_device_result
ast_radio_device_acquire(const struct ast_radio_device_request *request,
			 struct ast_radio_device **device);
