# Adapted from the Tcl wiki (http://wiki.tcl.tk)

package require html
namespace eval scgi {
    variable non_parsed_headers 0

    proc listen {port nph} {
        variable non_parsed_headers
        set non_parsed_headers $nph
        socket -server [namespace code connect] $port
    }

    proc connect {sock ip port} {
        fconfigure $sock -blocking 0 -translation {binary crlf}
        fileevent $sock readable [namespace code [list read_length $sock {}]]
    }

    proc read_length {sock data} {
        append data [read $sock]
        if {[eof $sock]} {
            close $sock
            return
        }
         
        set colonIdx [string first : $data]
        if {$colonIdx == -1} {
            # we don't have the headers length yet
            fileevent $sock readable [namespace code [list read_length $sock $data]]
            return
        } else {
            set length [string range $data 0 $colonIdx-1]
            set data [string range $data $colonIdx+1 end]
            read_headers $sock $length $data
        }
    }

    proc read_headers {sock length data} {
        append data [read $sock]
        
        if {[string length $data] < $length+1} {
            # we don't have the complete headers yet, wait for more
            fileevent $sock readable [namespace code [list read_headers $sock $length $data]]
            return
        } else {
            set headers [string range $data 0 $length-1]
            set headers [lrange [split $headers \0] 0 end-1]
            set body [string range $data $length+1 end]
            set content_length [dict get $headers CONTENT_LENGTH]
            read_body $sock $headers $content_length $body
        }
    }

    proc read_body {sock headers content_length body} {
        append body [read $sock]

        if {[string length $body] < $content_length} {
            # we don't have the complete body yet, wait for more
            fileevent $sock readable [namespace code [list read_body $sock $headers $content_length $body]]
            return
        } else {
            handle_request $sock $headers $body
        }
    }

    proc handle_request {sock headers body} {
        variable non_parsed_headers

        array set Headers $headers

        if {$non_parsed_headers} {
            puts -nonewline $sock "HTTP/1.0 200 OK\nContent-Type: text/html\n\n"
        } else {
            puts -nonewline $sock "Status: 200 OK\nContent-Type: text/html\n\n"
        }
        puts $sock "<HTML><BODY>"
        set Settings(Time) [clock format [clock seconds]]
        set Settings(Non\ parsed\ headers) $non_parsed_headers
        puts $sock "[::html::tableFromArray Settings] [::html::tableFromArray Headers]<H3>Body</H3><PRE>$body</PRE>"

        if {$Headers(REQUEST_METHOD) eq "GET"} {
            puts $sock "<FORM METHOD=\"post\" ACTION=\"$Headers(SCRIPT_NAME)\">"
            foreach pair [split $Headers(QUERY_STRING) &] {
                lassign [split $pair =] key val
                puts $sock "$key: [::html::textInput $key $val]<BR>"
            }
            puts $sock "<BR><INPUT TYPE='submit' VALUE='Try POST'></FORM>"
        } else {
            puts $sock "<FORM METHOD=\"get\" ACTION=\"$Headers(SCRIPT_NAME)\">"
            foreach pair [split $body &] {
                lassign [split $pair =] key val
                puts $sock "$key: [::html::textInput $key $val]<BR>"
            }
            puts $sock "<BR><INPUT TYPE='submit' VALUE='Try GET'></FORM>"
        }
        puts $sock "</BODY></HTML>"
        close $sock
    }
}

proc opt {k val} {
    if {[dict exists $::argv $k]} {
        return [dict get $::argv $k]
    } else {
        return $val
    }
}

scgi::listen [opt -port 9999] [opt -nph 0]

vwait forever
