/*
	write by lenge.wan@megafone.co at 2017.
*/

package co.megafone.leds;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.content.DialogInterface;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.view.View.OnClickListener;
import android.view.View;

import co.megafone.leds.Leds;

public class MainActivity extends Activity implements OnClickListener {
	private int index;
	private int rgb;
	private TextView set_tv1;
	private EditText set_et1;
	private TextView set_tv2;
	private EditText set_et2;
	private Button set_bt;
	
	private TextView get_tv1;
	private EditText get_et;
	private TextView get_tv2;
	private Button get_bt;
	
	private int status;
	private TextView voice_tv1;
	private EditText voice_et1;
	private TextView voice_tv2;
	private EditText voice_et2;
	private Button voice_bt;

	private Leds leds;
	
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_main);
		//showNormalDialog();
		
		set_tv1 = (TextView)findViewById(R.id.set_tv1);
		set_tv1.setText("index:");
		set_et1 = (EditText)findViewById(R.id.set_et1);
		set_tv2 = (TextView)findViewById(R.id.set_tv2);
		set_tv2.setText("RGB(aabbcc):");
		set_et2 = (EditText)findViewById(R.id.set_et2);
		set_bt = (Button)findViewById(R.id.set_bt);
		set_bt.setOnClickListener(this);
		set_bt.setText("ledSet");
		
		get_tv1 = (TextView)findViewById(R.id.get_tv1);
		get_tv1.setText("index:");
		get_et = (EditText)findViewById(R.id.get_et);
		get_tv2 = (TextView)findViewById(R.id.get_tv2);
		get_bt = (Button)findViewById(R.id.get_bt);
		get_bt.setOnClickListener(this);
		get_bt.setText("ledGet");
		
		voice_tv1 = (TextView)findViewById(R.id.voice_tv1);
		voice_tv1.setText("status:");
		voice_et1 = (EditText)findViewById(R.id.voice_et1);
		voice_tv2 = (TextView)findViewById(R.id.voice_tv2);
		voice_tv2.setText("index:");
		voice_et2 = (EditText)findViewById(R.id.voice_et2);
		voice_bt = (Button)findViewById(R.id.voice_bt);
		voice_bt.setOnClickListener(this);
		voice_bt.setText("voiceStatus");

		leds = new Leds();
	}
	
	@Override
    public void onClick(View v) {
		switch(v.getId()) {
		case R.id.set_bt:
			index = Integer.parseInt(set_et1.getText().toString());
			rgb = Integer.parseInt(set_et2.getText().toString(), 16);
			leds.setLed(index, rgb);
			break;
		case R.id.get_bt:
			index = Integer.parseInt(get_et.getText().toString());
			rgb = leds.getLed(index);
			get_tv2.setText(Integer.toHexString(rgb));
			break;
		case R.id.voice_bt:
			status = Integer.parseInt(voice_et1.getText().toString());
			index = Integer.parseInt(voice_et2.getText().toString());
			leds.setVoice(status, index);
			break;
		default:
			break;
		}
	}

/*
	private void showNormalDialog() {
		AlertDialog.Builder normalDialog = new AlertDialog.Builder(MainActivity.this);
		//normalDialog.setIcon(R.drawable.icon_dialog);
		normalDialog.setTitle("ledsTest");
		normalDialog.setMessage("ok");
		normalDialog.setPositiveButton("ok", new DialogInterface.OnClickListener() {
					@Override
					public void onClick(DialogInterface dialog, int which) {
						//...To-do
			                }
		});
		normalDialog.setNegativeButton("colse", new DialogInterface.OnClickListener() {
					@Override
					public void onClick(DialogInterface dialog, int which) {
						//...To-do
					}
		});
		normalDialog.show();
	}
*/
}
