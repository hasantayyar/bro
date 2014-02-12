# Generated by binpac_quickstart

refine flow MySQL_Flow += {
	function proc_mysql_handshakev10(msg: Handshake_v10): bool
		%{
		if ( mysql_server_version) 
			BifEvent::generate_mysql_server_version(connection()->bro_analyzer(), connection()->bro_analyzer()->Conn(),
								bytestring_to_val(${msg.server_version}));
		connection()->bro_analyzer()->ProtocolConfirmation();
		return true;
		%}

	function proc_mysql_handshake_response_packet(msg: Handshake_Response_Packet): bool
		%{
		BifEvent::generate_mysql_handshake_response(connection()->bro_analyzer(), connection()->bro_analyzer()->Conn(),
							    bytestring_to_val(${msg.username}));
		return true;
		%}

};

refine typeattr Handshake_v10 += &let {
	proc = $context.flow.proc_mysql_handshakev10(this);
};

refine typeattr Handshake_Response_Packet += &let {
	proc = $context.flow.proc_mysql_handshake_response_packet(this);
};