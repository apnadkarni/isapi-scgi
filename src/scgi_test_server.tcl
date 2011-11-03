# Adapted from the Tcl wiki (http://wiki.tcl.tk

package require html
puts "Starting..."
namespace eval scgi {
    proc listen {port} {
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
}

proc handle_request {sock headers body} {
    array set Headers $headers

    #parray Headers
    #puts $body
     puts $sock "HTTP/1.0 200 OK\nContent-Type: text/html\n\n<HTML><BODY>[::html::tableFromArray Headers]<H3>Body</H3><PRE>$body</PRE>"
    if {$Headers(REQUEST_METHOD) eq "GET"} {
        puts $sock {<FORM METHOD="post" ACTION="/sandbox/isapi_scgi.dll">}
        foreach pair [split $Headers(QUERY_STRING) &] {
            lassign [split $pair =] key val
            puts $sock "$key: [::html::textInput $key $val]<BR>"
        }
        puts $sock "<BR><INPUT TYPE='submit' VALUE='Try POST'></FORM>"
    } else {
        puts $sock {<FORM METHOD="get" ACTION="/sandbox/isapi_scgi.dll">}
        foreach pair [split $body &] {
            lassign [split $pair =] key val
            puts $sock "$key: [::html::textInput $key $val]<BR>"
        }
        puts $sock "<BR><INPUT TYPE='submit' VALUE='Try GET'></FORM>"
    }
    puts $sock "</BODY></HTML>"
    close $sock
}

if {[llength $::argv]} {
    scgi::listen [lindex $::argv 0]
} else {
    scgi::listen 9999
}
